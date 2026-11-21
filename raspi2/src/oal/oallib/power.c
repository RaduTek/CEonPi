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
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>
#include <oal.h>
#include <bsp.h>

// System timer free-running counter (1 MHz), mapped in timer.c
extern volatile PDWORD pTimerCntLowReg;

// startup.s: drain the write buffer, then CP15 wait-for-interrupt
void RaspiCpuWaitForInterrupt(void);

// Peripheral block (uncached static mapping, debug.c)
extern PBYTE g_p2835Regs;

// ---------------------------------------------------------------------------
// OALRaspiWatchdogReset: CUSTOM
//
// Resets the SoC through the PM watchdog. With fHalt the "halt" partition
// (63) is written to PM_RSTS first: bootcode.bin sees it after the reset and
// halts in a low-power state until the power is cycled (this is how Linux and
// FreeBSD power off a Pi). Without fHalt it is a normal full reset and the
// firmware boots kernel.img again.
//
// Never returns: the watchdog fires about 10 * 16 us after it is armed.
// Used by OEMPowerOff (halt) and OALIoCtlHalReboot (reset).
//
void OALRaspiWatchdogReset(BOOL fHalt)
{
    volatile DWORD *pRstc;
    volatile DWORD *pRsts;
    volatile DWORD *pWdog;
    DWORD dwVal;

    if (g_p2835Regs == 0)
        return;

    pRstc = (volatile DWORD *)(g_p2835Regs + PM_BASEOFF + PM_RSTC_OFFSET);
    pRsts = (volatile DWORD *)(g_p2835Regs + PM_BASEOFF + PM_RSTS_OFFSET);
    pWdog = (volatile DWORD *)(g_p2835Regs + PM_BASEOFF + PM_WDOG_OFFSET);

    if (fHalt)
    {
        // Mark partition 63 so the firmware halts instead of booting
        dwVal = *pRsts;
        *pRsts = PM_PASSWORD | dwVal | PM_RSTS_HALT;
    }

    // Arm the watchdog with a short timeout, then select a full reset
    *pWdog = PM_PASSWORD | (10 & PM_WDOG_TIME_SET);
    dwVal = *pRstc;
    *pRstc = PM_PASSWORD | (dwVal & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;

    for (;;)
    {
        // wait for the reset
    }
}

// ---------------------------------------------------------------------------
// OALIoCtlHalReboot: IOCTL_HAL_REBOOT handler (table entry in ioctl.c)
//
// Called by the power manager for POWER_STATE_RESET, or directly through
// KernelIoControl. Registered with OAL_IOCTL_FLAG_NOCS: it never returns, so
// it must not hold the IOCTL critical section.
//
BOOL OALIoCtlHalReboot(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                       VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    UNREFERENCED_PARAMETER(code);
    UNREFERENCED_PARAMETER(pInpBuffer);
    UNREFERENCED_PARAMETER(inpSize);
    UNREFERENCED_PARAMETER(pOutBuffer);
    UNREFERENCED_PARAMETER(outSize);
    UNREFERENCED_PARAMETER(pOutSize);

    OALMSG(1, (L"OALIoCtlHalReboot: resetting\r\n"));
    OALRaspiWatchdogReset(FALSE);

    // Only reached if the peripherals are not mapped
    return FALSE;
}

// ---------------------------------------------------------------------------
// OEMIdle: REQUIRED
//
// This function is called by the kernel to place the CPU in the idle state
// when there are no threads ready to run.
//
// The kernel calls this with interrupts disabled. On the ARM1176 a pending
// IRQ still wakes the core from WFI even while CPSR.I is set; WFI returns
// and the kernel then re-enables interrupts so the ISR runs. The 1 ms
// scheduler tick (Timer 3) guarantees we wake at least once per millisecond.
//
// Idle time is accumulated in curridlehigh:curridlelow in system timer
// counts (1 us); idleconv (set in OALTimerInit) is counts per ms, so the
// kernel's GetIdleTime() reports milliseconds.
//
void OEMIdle(DWORD dwIdleParam)
{
    DWORD dwStart;
    DWORD dwSlept;
    ULARGE_INTEGER liIdle;

    if (pTimerCntLowReg == 0)
    {
        // Timer not mapped yet; still idle, just do not account the time.
        RaspiCpuWaitForInterrupt();
        return;
    }

    dwStart = *(volatile DWORD *)pTimerCntLowReg;

    RaspiCpuWaitForInterrupt();

    // Unsigned subtraction handles the 32-bit counter wrap.
    dwSlept = *(volatile DWORD *)pTimerCntLowReg - dwStart;

    liIdle.LowPart = curridlelow;
    liIdle.HighPart = curridlehigh;
    liIdle.QuadPart += dwSlept;
    curridlelow = liIdle.LowPart;
    curridlehigh = liIdle.HighPart;

    return;
}

// ---------------------------------------------------------------------------
// OEMPowerOff: REQUIRED
//
// The function is responsible for any final power-down state and for
// placing the CPU into a suspend state.
//
// The BCM2835 cannot suspend, so this is the BSP's shutdown: the kernel gets
// here from the Suspend path (Start > Suspend, SetSystemPowerState
// POWER_STATE_SUSPEND) after the power manager has powered down the drivers
// and notified the file systems. We halt the board through the firmware
// (watchdog reset into partition 63) and never return. The PM suspend
// timeouts are set to 0 in platform.reg so this never happens on idle.
//
void OEMPowerOff(void)
{
    OALMSG(1, (L"OEMPowerOff: halting (shutdown)\r\n"));
    OALRaspiWatchdogReset(TRUE);

    // Only reached if the peripherals are not mapped
    return;
}

// ---------------------------------------------------------------------------
// OEMHaltSystem: OPTIONAL
//
// The function is called when the kernel is about to halt the system.
//
void OEMHaltSystem(void)
{
  // Fill in halt code here.

  return;
}

// ---------------------------------------------------------------------------
// OEMIdleEx: OPTIONAL
//
// This function is called by the kernel to place the CPU(s) in the idle state
// when there are no threads ready to run.  It is required for
// multiprocessor support if there is more than one CPU to handle per
// CPUIdle.  OEMIdleEx is more performant than OEMIdle, and if you implement
// OEMIdleEx, you can leave OEMIdle as a stub.
//
void OEMIdleEx(LARGE_INTEGER *pliIdleTime)
{
  // fill in idle code here

  return;
}


