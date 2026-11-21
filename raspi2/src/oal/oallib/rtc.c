//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
//
// Use of this sample source code is subject to the terms of the Microsoft
// license agreement under which you licensed this sample source code. If
// you did not accept the terms of the license agreement, you are not
// authorized to use this sample source code. For the terms of the license,
// please see the license agreement between you and Microsoft or, if applicable,
// see the LICENSE.RTF on your install media or the root of your tools installation.
// THE SAMPLE SOURCE CODE IS PROVIDED "AS IS", WITH NO WARRANTIES OR INDEMNITIES.
//
//------------------------------------------------------------------------------
//
// rtc.c - software real-time clock
//
// The Raspberry Pi 1 has no battery-backed RTC. The clock is kept from the
// BCM2835 system timer, a free-running 64-bit counter at 1 MHz (it never
// wraps in practice):
//
//     now = base time + (counter now - counter at base) * 10   [FILETIME]
//
// The base starts at the image build date (00:00, from __DATE__) on the
// first call after boot, and moves whenever the kernel sets the time
// (Date/Time control panel, SetLocalTime/SetSystemTime). Nothing persists
// across a reboot. As usual in CE, the RTC holds local time.
//
// The alarm is kept as a counter deadline; the 1 ms scheduler tick ISR
// (intr.c) calls OALRtcCheckAlarm and returns SYSINTR_RTC_ALARM when it
// passes. There is no RTC interrupt hardware.
//
// Pattern follows the stock timer-based soft RTC
// (platform\common\src\common\rtc\timer\rtc.c): NKSystemTimeToFileTime /
// NKFileTimeToSystemTime for conversion, INTERRUPTS_ENABLE to keep the
// 64-bit state consistent against the ISR.
//
#include <windows.h>
#include <oemglobal.h>

#include <nkintr.h>
#include <oal.h>
#include <bsp.h>

//------------------------------------------------------------------------------
// Externs
//
extern PBYTE g_p2835Regs;

//------------------------------------------------------------------------------
// Local state
//
static BOOL      g_fRtcInit = FALSE;    // base set (lazily, first use)
static ULONGLONG g_ullBaseFt = 0;       // local time at base, FILETIME units
static ULONGLONG g_ullBaseCnt = 0;      // system timer count at base (1 MHz)

static BOOL      g_fAlarmSet = FALSE;   // an alarm time is stored
static ULONGLONG g_ullAlarmFt = 0;      // alarm local time, FILETIME units
static volatile BOOL g_fAlarmArmed = FALSE; // ISR may fire the alarm
static ULONGLONG g_ullAlarmCnt = 0;     // alarm deadline as timer count

// Fallback base if __DATE__ cannot be parsed
#define RTC_DEFAULT_YEAR    2026
#define RTC_DEFAULT_MONTH   1
#define RTC_DEFAULT_DAY     1

// ---------------------------------------------------------------------------
// ReadSysTimer64: 64-bit system timer count (1 MHz). Re-reads the low word
// if the high word changed while reading. Returns 0 if not mapped.
//
static ULONGLONG ReadSysTimer64(void)
{
    volatile DWORD *pLo;
    volatile DWORD *pHi;
    DWORD dwHi, dwLo, dwHi2;

    if (g_p2835Regs == 0)
        return 0;

    pLo = (volatile DWORD *)(g_p2835Regs + SYSTEM_TIMER_BASEOFF + TIMER_CHL_OFFSET);
    pHi = (volatile DWORD *)(g_p2835Regs + SYSTEM_TIMER_BASEOFF + TIMER_CHI_OFFSET);

    dwHi = *pHi;
    dwLo = *pLo;
    dwHi2 = *pHi;
    if (dwHi2 != dwHi)
    {
        dwHi = dwHi2;
        dwLo = *pLo;
    }
    return ((ULONGLONG)dwHi << 32) | dwLo;
}

// ---------------------------------------------------------------------------
// FileTime helpers
//
static BOOL SystemTimeToFt(const SYSTEMTIME *pst, ULONGLONG *pFt)
{
    FILETIME ft;

    if (!NKSystemTimeToFileTime(pst, &ft))
        return FALSE;
    *pFt = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return TRUE;
}

// ---------------------------------------------------------------------------
// ParseBuildDate: fill pst with the __DATE__ ("Mmm dd yyyy") date at 00:00.
//
static BOOL ParseBuildDate(SYSTEMTIME *pst)
{
    static const char szMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *pszDate = __DATE__;
    int i, iMonth = 0, iDay = 0, iYear = 0;

    for (i = 0; i < 12; i++)
    {
        if (pszDate[0] == szMonths[i * 3] &&
            pszDate[1] == szMonths[i * 3 + 1] &&
            pszDate[2] == szMonths[i * 3 + 2])
        {
            iMonth = i + 1;
            break;
        }
    }

    // Day is space-padded: "Nov  5 2026"
    for (i = 4; i <= 5; i++)
    {
        if (pszDate[i] >= '0' && pszDate[i] <= '9')
            iDay = iDay * 10 + (pszDate[i] - '0');
    }

    for (i = 7; i <= 10; i++)
    {
        if (pszDate[i] < '0' || pszDate[i] > '9')
            return FALSE;
        iYear = iYear * 10 + (pszDate[i] - '0');
    }

    if (iMonth == 0 || iDay < 1 || iDay > 31)
        return FALSE;

    memset(pst, 0, sizeof(*pst));
    pst->wYear = (WORD)iYear;
    pst->wMonth = (WORD)iMonth;
    pst->wDay = (WORD)iDay;
    return TRUE;
}

// ---------------------------------------------------------------------------
// RtcEnsureInit: set the base to the build date the first time it is needed.
// Kept out of OEMInit so nothing here runs before the kernel is up.
//
static BOOL RtcEnsureInit(void)
{
    SYSTEMTIME st;
    ULONGLONG ullFt;
    BOOL fEnabled;

    if (g_fRtcInit)
        return TRUE;

    if (g_p2835Regs == 0)
        return FALSE;

    if (!ParseBuildDate(&st))
    {
        memset(&st, 0, sizeof(st));
        st.wYear = RTC_DEFAULT_YEAR;
        st.wMonth = RTC_DEFAULT_MONTH;
        st.wDay = RTC_DEFAULT_DAY;
    }

    if (!SystemTimeToFt(&st, &ullFt))
        return FALSE;

    fEnabled = INTERRUPTS_ENABLE(FALSE);
    if (!g_fRtcInit)
    {
        g_ullBaseFt = ullFt;
        g_ullBaseCnt = ReadSysTimer64();
        g_fRtcInit = TRUE;
    }
    INTERRUPTS_ENABLE(fEnabled);

    OALMSG(OAL_RTC&&OAL_FUNC, (L"RTC: base %d-%02d-%02d 00:00 (build date)\r\n",
        st.wYear, st.wMonth, st.wDay));
    return TRUE;
}

// ---------------------------------------------------------------------------
// RtcArmAlarmLocked: recompute the alarm deadline from the stored alarm time
// and the current base. Call with interrupts disabled. An alarm already in
// the past fires on the next tick.
//
static void RtcArmAlarmLocked(void)
{
    ULONGLONG ullNowCnt;

    g_fAlarmArmed = FALSE;
    if (!g_fAlarmSet)
        return;

    ullNowCnt = ReadSysTimer64();
    if (g_ullAlarmFt <= g_ullBaseFt)
        g_ullAlarmCnt = ullNowCnt;
    else
        g_ullAlarmCnt = g_ullBaseCnt + (g_ullAlarmFt - g_ullBaseFt) / 10;

    if (g_ullAlarmCnt < ullNowCnt)
        g_ullAlarmCnt = ullNowCnt;

    g_fAlarmArmed = TRUE;
}

// ---------------------------------------------------------------------------
// OEMInitRTC: OPTIONAL
//
// Called from OEMInit. The software clock initializes lazily on first use
// (RtcEnsureInit), so there is nothing to do here.
//
void InitRTC(void)
{
    return;
}

// ---------------------------------------------------------------------------
// OEMGetRealTime: REQUIRED
//
// This function is called by the kernel to retrieve the time from the
// real-time clock.
//
BOOL OEMGetRealTime(LPSYSTEMTIME lpst)
{
    BOOL rc = FALSE;
    BOOL fEnabled;
    ULONGLONG ullBaseFt, ullBaseCnt, ullCnt, ullNowFt;
    FILETIME ft;

    OALMSG(OAL_RTC&&OAL_FUNC, (L"+OEMGetRealTime(pTime = 0x%x)\r\n", lpst));

    if (lpst == NULL)
        goto cleanUp;

    if (!RtcEnsureInit())
        goto cleanUp;

    fEnabled = INTERRUPTS_ENABLE(FALSE);
    ullBaseFt = g_ullBaseFt;
    ullBaseCnt = g_ullBaseCnt;
    ullCnt = ReadSysTimer64();
    INTERRUPTS_ENABLE(fEnabled);

    // 1 count = 1 us = 10 FILETIME units
    ullNowFt = ullBaseFt + (ullCnt - ullBaseCnt) * 10;
    ft.dwLowDateTime = (DWORD)ullNowFt;
    ft.dwHighDateTime = (DWORD)(ullNowFt >> 32);

    rc = NKFileTimeToSystemTime(&ft, lpst);

cleanUp:
    OALMSG(OAL_RTC&&OAL_FUNC, (L"-OEMGetRealTime(rc = %d)\r\n", rc));
    return rc;
}

// ---------------------------------------------------------------------------
// OEMSetRealTime: REQUIRED
//
// This function is called by the kernel to set the real-time clock.
//
BOOL OEMSetRealTime(LPSYSTEMTIME lpst)
{
    BOOL rc = FALSE;
    BOOL fEnabled;
    ULONGLONG ullFt;

    if (lpst == NULL)
        goto cleanUp;

    OALMSG(OAL_RTC&&OAL_FUNC, (L"+OEMSetRealTime(%d/%d/%d %d:%d:%d.%03d)\r\n",
        lpst->wYear, lpst->wMonth, lpst->wDay, lpst->wHour, lpst->wMinute,
        lpst->wSecond, lpst->wMilliseconds));

    if (!RtcEnsureInit())
        goto cleanUp;

    // Rejects out-of-range fields
    if (!SystemTimeToFt(lpst, &ullFt))
        goto cleanUp;

    fEnabled = INTERRUPTS_ENABLE(FALSE);
    g_ullBaseFt = ullFt;
    g_ullBaseCnt = ReadSysTimer64();
    RtcArmAlarmLocked();
    INTERRUPTS_ENABLE(fEnabled);

    rc = TRUE;

cleanUp:
    OALMSG(OAL_RTC&&OAL_FUNC, (L"-OEMSetRealTime(rc = %d)\r\n", rc));
    return rc;
}

// ---------------------------------------------------------------------------
// OEMSetAlarmTime: REQUIRED
//
// This function is called by the kernel to set the real-time clock alarm.
//
BOOL OEMSetAlarmTime(LPSYSTEMTIME lpst)
{
    BOOL rc = FALSE;
    BOOL fEnabled;
    ULONGLONG ullFt;

    if (lpst == NULL)
        goto cleanUp;

    OALMSG(OAL_RTC&&OAL_FUNC, (L"+OEMSetAlarmTime(%d/%d/%d %d:%d:%d.%03d)\r\n",
        lpst->wYear, lpst->wMonth, lpst->wDay, lpst->wHour, lpst->wMinute,
        lpst->wSecond, lpst->wMilliseconds));

    if (!RtcEnsureInit())
        goto cleanUp;

    if (!SystemTimeToFt(lpst, &ullFt))
        goto cleanUp;

    fEnabled = INTERRUPTS_ENABLE(FALSE);
    g_ullAlarmFt = ullFt;
    g_fAlarmSet = TRUE;
    RtcArmAlarmLocked();
    INTERRUPTS_ENABLE(fEnabled);

    rc = TRUE;

cleanUp:
    OALMSG(OAL_RTC&&OAL_FUNC, (L"-OEMSetAlarmTime(rc = %d)\r\n", rc));
    return rc;
}

// ---------------------------------------------------------------------------
// OALRtcCheckAlarm: CUSTOM, called from the scheduler tick ISR (intr.c).
//
// Returns SYSINTR_RTC_ALARM once when the alarm deadline has passed,
// otherwise SYSINTR_NOP. The alarm is one-shot: the kernel sets the next one
// with OEMSetAlarmTime.
//
DWORD OALRtcCheckAlarm(void)
{
    if (!g_fAlarmArmed)
        return SYSINTR_NOP;

    if (ReadSysTimer64() < g_ullAlarmCnt)
        return SYSINTR_NOP;

    g_fAlarmArmed = FALSE;
    g_fAlarmSet = FALSE;
    return SYSINTR_RTC_ALARM;
}
