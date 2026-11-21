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
void OEMPowerOff(void)
{
  // Fill in power off code here.

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


