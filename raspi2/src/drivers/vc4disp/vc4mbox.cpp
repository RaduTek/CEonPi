//
// vc4mbox.cpp - firmware mailbox calls for vc4disp (CEonPi)
//
// MailboxTags sends a tag list synchronously through the OAL
// (IOCTL_HAL_MY_MAILBOX_PROPERTY; the OAL busy-waits ~2 ms for the reply).
//
// The mailbox thread sends the frequent updates without blocking their
// callers: the newest hardware cursor state (vc4cursor.cpp) and the newest
// overlay plane state (vc4overlay.cpp). Callers store the state under
// m_csMbox and wake the thread, which builds one property message holding
// a "Set cursor state" tag, a "Set plane" tag or both, posts it through the
// OAL's asynchronous mailbox IOCTL from its own uncached buffer and polls
// for the reply with Sleep(1) in between, leaving the CPU to other threads.
// The OAL can track only one pending asynchronous message, hence one thread
// for both. Cursor moves go out at most once per HwCursorInterval ms (the
// moves in between are merged); plane updates go out at once. Without the
// asynchronous IOCTL (an older OAL), or after a reply timeout, the thread
// falls back to the synchronous call for good.
//
// Statistics (1 MHz system timer) are logged every VC4_CURSOR_LOG_CALLS
// cursor calls and every VC4_OVL_LOG_CALLS plane updates.
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <image_cfg.h>
#include "vc4disp.h"

#define TAG_RESPONSE_OK         0x80000000
#define PROPERTY_RESPONSE_OK    0x80000000

#define BUS_UNCACHED_ALIAS      0xC0000000

// Message buffer: one page, uncached (a message is at most ~30 DWORDs)
#define MBOX_MSG_ALLOC          PAGE_SIZE

// Longest wait for an asynchronous reply, in 1 ms polls
#define MBOX_ASYNC_POLLS        100

// Thread priority: above applications, below GWES's input threads
#define MBOX_THREAD_PRIO        110

// Calls between two statistics lines (each KITL line costs about 30 ms)
#define VC4_CURSOR_LOG_CALLS    512
#define VC4_OVL_LOG_CALLS       600

// BCM2835 system timer: counter low word at +4, 1 MHz
#define SYSTIMER_PA_BASE        0x20003000
#define SYSTIMER_CLO_DWORD      1

//------------------------------------------------------------------------------
//
//  Method:  MailboxTags
//
//  Sends a tag list through the OAL and checks that every tag was answered.
//  pTags is overwritten with the response. Not for "Set plane": the
//  firmware leaves that tag's response bit clear even when it works.
//
BOOL Vc4GPE::MailboxTags(DWORD *pTags, DWORD cbTags)
{
    DWORD cbOut = 0, iWord, nWords = cbTags / sizeof(DWORD);

    if (!KernelIoControl(
        IOCTL_HAL_MY_MAILBOX_PROPERTY, pTags, cbTags, pTags, cbTags, &cbOut
    )) {
        return FALSE;
    }

    // Every tag must carry the response bit
    for (iWord = 0; iWord + 3 <= nWords; iWord += 3 + pTags[iWord + 1] / 4) {
        if ((pTags[iWord + 2] & TAG_RESPONSE_OK) == 0) return FALSE;
    }
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  MailboxGetValue
//
//  Sends one tag with a one-DWORD value buffer (dwIn) and returns the
//  answered value.
//
BOOL Vc4GPE::MailboxGetValue(DWORD dwTag, DWORD dwIn, DWORD *pdwValue)
{
    DWORD tags[4];

    tags[0] = dwTag;
    tags[1] = sizeof(DWORD);
    tags[2] = 0;
    tags[3] = dwIn;
    if (!MailboxTags(tags, sizeof(tags))) return FALSE;
    *pdwValue = tags[3];
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  MboxInit
//
//  Allocates the message buffer and starts the mailbox thread, once.
//  Called by the cursor and by the overlay, possibly from two threads.
//
BOOL Vc4GPE::MboxInit()
{
    ULONG pa = 0;
    BOOL fOk = FALSE;

    EnterCriticalSection(&m_csMbox);
    if (m_hMboxThread != NULL) {
        fOk = TRUE;
        goto cleanUp;
    }

    if (m_pMboxMsg == NULL) {
        m_pMboxMsg = (DWORD*)AllocPhysMem(
            MBOX_MSG_ALLOC, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa
        );
        if (m_pMboxMsg == NULL) {
            RETAILMSG(1, (L"vc4disp: mailbox message allocation failed\r\n"));
            goto cleanUp;
        }
        m_dwMboxMsgBus = BUS_UNCACHED_ALIAS | pa;
    }

    // System timer for the call statistics (optional)
    if (m_pSysTimer == NULL) {
        PHYSICAL_ADDRESS phys;
        phys.QuadPart = SYSTIMER_PA_BASE;
        m_pSysTimer = (volatile DWORD*)MmMapIoSpace(phys, PAGE_SIZE, FALSE);
    }

    if (m_hOvlDoneEvent == NULL) {
        m_hOvlDoneEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    if (m_hMboxEvent == NULL) {
        m_hMboxEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    if (m_hMboxEvent == NULL || m_hOvlDoneEvent == NULL) {
        RETAILMSG(1, (L"vc4disp: mailbox event creation failed\r\n"));
        goto cleanUp;
    }

    m_fMboxExit = FALSE;
    m_hMboxThread = CreateThread(NULL, 0, MboxThreadProc, this, 0, NULL);
    if (m_hMboxThread == NULL) {
        RETAILMSG(1, (L"vc4disp: mailbox thread creation failed\r\n"));
        goto cleanUp;
    }
    CeSetThreadPriority(m_hMboxThread, MBOX_THREAD_PRIO);
    fOk = TRUE;

cleanUp:
    LeaveCriticalSection(&m_csMbox);
    return fOk;
}

//------------------------------------------------------------------------------
//
//  Method:  MboxDeinit
//
//  Stops the mailbox thread and releases what MboxInit set up (the overlay
//  done event is closed by the destructor).
//
VOID Vc4GPE::MboxDeinit()
{
    if (m_hMboxThread != NULL) {
        m_fMboxExit = TRUE;
        SetEvent(m_hMboxEvent);
        WaitForSingleObject(m_hMboxThread, 1000);
        CloseHandle(m_hMboxThread);
        m_hMboxThread = NULL;
    }
    if (m_hMboxEvent != NULL) {
        CloseHandle(m_hMboxEvent);
        m_hMboxEvent = NULL;
    }
    if (m_pSysTimer != NULL) {
        MmUnmapIoSpace((PVOID)m_pSysTimer, PAGE_SIZE);
        m_pSysTimer = NULL;
    }
    if (m_pMboxMsg != NULL) {
        FreePhysMem(m_pMboxMsg);
        m_pMboxMsg = NULL;
    }
}

//------------------------------------------------------------------------------
//
//  Method:  MboxWake
//
VOID Vc4GPE::MboxWake()
{
    if (m_hMboxEvent != NULL) SetEvent(m_hMboxEvent);
}

//------------------------------------------------------------------------------
//
//  Method:  MboxSend
//
//  Sends the complete property message in m_pMboxMsg (cbMsg bytes: size,
//  request code, tags, end tag) and waits for the reply, sleeping. The reply
//  replaces the message. Returns FALSE if the message as a whole failed.
//
BOOL Vc4GPE::MboxSend(DWORD cbMsg)
{
    RASPI_MAILBOX_ASYNC req;
    DWORD *pMsg = m_pMboxMsg;
    DWORD dwDone = 0, cbTags, cbOut = 0, i;

    if (m_fMailboxAsync) {
        req.dwCommand = RASPI_MAILBOX_ASYNC_SEND;
        req.dwBusAddr = m_dwMboxMsgBus;
        req.pvMessage = pMsg;
        req.cbMessage = cbMsg;
        if (KernelIoControl(IOCTL_HAL_MY_MAILBOX_ASYNC, &req, sizeof(req), NULL, 0, NULL)) {
            req.dwCommand = RASPI_MAILBOX_ASYNC_POLL;
            for (i = 0; i < MBOX_ASYNC_POLLS; i++) {
                Sleep(1);
                if (!KernelIoControl(IOCTL_HAL_MY_MAILBOX_ASYNC, &req, sizeof(req),
                        &dwDone, sizeof(dwDone), NULL)) {
                    break;
                }
                if (dwDone) return (pMsg[1] == PROPERTY_RESPONSE_OK);
            }

            // No reply: forget the message and stay synchronous from now on,
            // so a late reply can never be taken for a new message's
            req.dwCommand = RASPI_MAILBOX_ASYNC_ABANDON;
            KernelIoControl(IOCTL_HAL_MY_MAILBOX_ASYNC, &req, sizeof(req), NULL, 0, NULL);
            RETAILMSG(1, (L"vc4disp: asynchronous mailbox message got no reply, "
                L"using synchronous calls\r\n"
            ));
        } else {
            RETAILMSG(1, (L"vc4disp: asynchronous mailbox unavailable (error %d), "
                L"using synchronous calls\r\n", GetLastError()
            ));
        }
        m_fMailboxAsync = FALSE;
    }

    // Synchronous: the OAL wants the tags only (no size, code or end tag)
    cbTags = cbMsg - 3 * sizeof(DWORD);
    if (!KernelIoControl(IOCTL_HAL_MY_MAILBOX_PROPERTY,
            &pMsg[2], cbTags, &pMsg[2], cbTags, &cbOut)) {
        return FALSE;
    }
    pMsg[1] = PROPERTY_RESPONSE_OK;
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  MboxThreadProc / MboxThread
//
DWORD WINAPI Vc4GPE::MboxThreadProc(LPVOID pContext)
{
    ((Vc4GPE*)pContext)->MboxThread();
    return 0;
}

VOID Vc4GPE::MboxThread()
{
    DWORD *pMsg = m_pMboxMsg;
    DWORD dwWait, dwNow, dwSince, nWords, iCursor, dwSeq, dwStart, dwUs, nMoves;
    BOOL fCursor, fPlane, fVisible, fOk;
    int x, y;

    while (!m_fMboxExit) {
        dwWait = INFINITE;
        fCursor = FALSE;
        fPlane = FALSE;
        nWords = 2;
        iCursor = 0;
        dwSeq = 0;
        fVisible = FALSE;
        x = 0;
        y = 0;

        // Take the newest states and build the message
        EnterCriticalSection(&m_csMbox);
        if (m_fCursorPending && !m_fHwCursorFailed) {
            dwNow = GetTickCount();
            dwSince = dwNow - m_dwCursorLastMs;
            if (dwSince >= m_dwCursorIntervalMs) {
                fCursor = TRUE;
                fVisible = m_fCursorVisible;
                x = m_xCursor;
                y = m_yCursor;
                m_fCursorPending = FALSE;
            } else {
                dwWait = m_dwCursorIntervalMs - dwSince;
            }
        }
        if (fCursor) {
            iCursor = nWords;
            nWords += HwBuildCursorTag(&pMsg[nWords], fVisible, x, y);
        }
        if (m_fOvlPending) {
            fPlane = TRUE;
            m_fOvlPending = FALSE;
            dwSeq = m_dwOvlSeqQueued;
            nWords += OverlayBuildTag(&pMsg[nWords]);
        }
        LeaveCriticalSection(&m_csMbox);

        if (!fCursor && !fPlane) {
            WaitForSingleObject(m_hMboxEvent, dwWait);
            continue;
        }

        pMsg[nWords++] = 0;                 // end tag
        pMsg[0] = nWords * sizeof(DWORD);
        pMsg[1] = 0;

        // The buffer is uncached: make sure the writes have left the write
        // buffer before the VideoCore reads it
        CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);

        dwStart = (m_pSysTimer != NULL) ? m_pSysTimer[SYSTIMER_CLO_DWORD] : 0;
        fOk = MboxSend(nWords * sizeof(DWORD));
        dwUs = (m_pSysTimer != NULL) ? m_pSysTimer[SYSTIMER_CLO_DWORD] - dwStart : 0;

        if (fCursor) {
            m_dwCursorLastMs = GetTickCount();
            if (!fOk || (pMsg[iCursor + 2] & TAG_RESPONSE_OK) == 0 ||
                pMsg[iCursor + 3] != 0
            ) {
                // MovePointer switches to the software cursor
                RETAILMSG(1, (L"vc4disp: Set cursor state rejected (resp 0x%08x "
                    L"0x%08x value %d)\r\n",
                    pMsg[1], pMsg[iCursor + 2], pMsg[iCursor + 3]
                ));
                m_fHwCursorFailed = TRUE;
            } else {
                m_nCursorCalls++;
                m_dwCursorUsTotal += dwUs;
                if (dwUs > m_dwCursorUsMax) m_dwCursorUsMax = dwUs;
                if (m_nCursorCalls >= VC4_CURSOR_LOG_CALLS) {
                    EnterCriticalSection(&m_csMbox);
                    nMoves = m_nCursorMoves;
                    m_nCursorMoves = 0;
                    LeaveCriticalSection(&m_csMbox);
                    RETAILMSG(1, (L"vc4disp: cursor: %d firmware calls for %d moves, "
                        L"call avg %d us, max %d us, interval %d ms, %s\r\n",
                        m_nCursorCalls, nMoves, m_dwCursorUsTotal / m_nCursorCalls,
                        m_dwCursorUsMax, m_dwCursorIntervalMs,
                        m_fMailboxAsync ? L"async" : L"sync"
                    ));
                    m_nCursorCalls = 0;
                    m_dwCursorUsTotal = 0;
                    m_dwCursorUsMax = 0;
                }
            }
        }

        if (fPlane) {
            // The firmware leaves "Set plane"'s response bit clear, so only
            // the message as a whole can be checked
            if (!fOk) {
                RETAILMSG(1, (L"vc4disp: overlay: Set plane message failed "
                    L"(resp 0x%08x)\r\n", pMsg[1]
                ));
            }
            m_nOvlCalls++;
            m_dwOvlUsTotal += dwUs;
            if (dwUs > m_dwOvlUsMax) m_dwOvlUsMax = dwUs;
            if (m_nOvlCalls >= VC4_OVL_LOG_CALLS) {
                RETAILMSG(1, (L"vc4disp: overlay: %d plane updates, call avg %d us, "
                    L"max %d us, %s\r\n",
                    m_nOvlCalls, m_dwOvlUsTotal / m_nOvlCalls, m_dwOvlUsMax,
                    m_fMailboxAsync ? L"async" : L"sync"
                ));
                m_nOvlCalls = 0;
                m_dwOvlUsTotal = 0;
                m_dwOvlUsMax = 0;
            }

            // Wake whoever waits for this update (even after a failure, so
            // no caller hangs)
            m_dwOvlSeqDone = dwSeq;
            SetEvent(m_hOvlDoneEvent);
        }
    }
}
