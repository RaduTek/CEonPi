//
// dwctt.h
//
// CDwcTt: one USB 2.0 transaction translator (TT), i.e. one high speed hub
// port that bridges to full/low speed devices below it. hcd2lib creates one
// per full/low speed device behind a high speed hub through
// CHcd::AddedTt(hubAddress, port) and hands it back to the pipe factories as
// the opaque ttContext, and to DeleteTt on teardown.
//
// Control and bulk splits only need the TT's identity (its hub address and
// port). Interrupt (periodic) splits also need a little bandwidth bookkeeping
// so their start-splits are spread across the eight microframes of a frame
// instead of all landing together: Reserve()/Release() keep a coarse per-
// microframe byte budget and hand each periodic pipe a start microframe.
//
#ifndef __DWCTT_H__
#define __DWCTT_H__

#include <windows.h>

class CDwcTt
{
public:
    CDwcTt(UCHAR bHubAddress, UCHAR bHubPort);

    UCHAR HubAddress(void) const { return m_bHubAddress; }
    UCHAR HubPort(void) const    { return m_bHubPort; }

    // Reserve budget for one periodic (interrupt) split endpoint that moves at
    // most dwBytes per poll. Returns the start microframe (0..5) the pipe
    // should launch its start-split in, or -1 if the TT has no room. Release()
    // gives the same budget back; pass the start microframe Reserve() returned.
    int  Reserve(DWORD dwBytes);
    void Release(int iStartUframe, DWORD dwBytes);

private:
    const UCHAR m_bHubAddress;
    const UCHAR m_bHubPort;
    // Bytes committed to each microframe. A full/low speed frame below the TT
    // carries roughly 188 bytes per microframe; only 0..5 are used as start
    // microframes so the complete-splits in the following microframes still
    // fit inside the frame.
    DWORD       m_adwBytes[8];
};

#endif // __DWCTT_H__
