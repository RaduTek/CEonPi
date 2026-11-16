//
// dwctt.cpp
//
// CDwcTt: bandwidth bookkeeping for periodic (interrupt) split transactions
// behind one high speed hub's transaction translator. See dwctt.h.
//
// This is a deliberately coarse port of the dwc2 microframe scheduler
// (drivers/usb/dwc2/hcd_queue.c). It tracks only a per-microframe byte budget
// and hands each periodic split endpoint a start microframe; it does not model
// the separate HS and FS/LS maps or the exact complete-split microframe mask.
// For the low-bandwidth HID and full speed devices this driver targets that is
// enough to keep start-splits spread out and to refuse gross overcommitment.
// One CDwcTt exists per (hub, port), so a multi-port hub is budgeted per port.
//
#include <windows.h>
#include "dwctt.h"

// Approximate full/low speed payload budget below a TT, in bytes per
// microframe (the classic ~187.5 B figure rounded down).
#define DWC_TT_BYTES_PER_UFRAME     188

// Start-splits are placed only in microframes 0..5, leaving 6 and 7 for the
// trailing complete-splits so a transaction cannot run off the end of a frame.
#define DWC_TT_MAX_START_UFRAME     5

CDwcTt::CDwcTt(UCHAR bHubAddress, UCHAR bHubPort)
    : m_bHubAddress(bHubAddress)
    , m_bHubPort(bHubPort)
{
    int i;
    for (i = 0; i < 8; i++)
        m_adwBytes[i] = 0;
}

int CDwcTt::Reserve(DWORD dwBytes)
{
    int iBest = -1;
    DWORD dwBestBytes = 0;
    int i;

    if (dwBytes == 0)
        dwBytes = 1;

    // Pick the least-loaded start microframe that still has room.
    for (i = 0; i <= DWC_TT_MAX_START_UFRAME; i++)
    {
        if (m_adwBytes[i] + dwBytes > DWC_TT_BYTES_PER_UFRAME)
            continue;
        if (iBest < 0 || m_adwBytes[i] < dwBestBytes)
        {
            iBest = i;
            dwBestBytes = m_adwBytes[i];
        }
    }

    if (iBest < 0)
        return -1;          // no room on this TT

    m_adwBytes[iBest] += dwBytes;
    return iBest;
}

void CDwcTt::Release(int iStartUframe, DWORD dwBytes)
{
    if (iStartUframe < 0 || iStartUframe >= 8)
        return;
    if (dwBytes == 0)
        dwBytes = 1;

    if (m_adwBytes[iStartUframe] >= dwBytes)
        m_adwBytes[iStartUframe] -= dwBytes;
    else
        m_adwBytes[iStartUframe] = 0;
}
