//
// dwctt.h
//
// CDwcTt: one USB 2.0 transaction translator (TT), i.e. one high speed hub
// port that bridges to full/low speed devices below it. hcd2lib creates one
// per full/low speed device behind a high speed hub through
// CHcd::AddedTt(hubAddress, port) and hands it back to the pipe factories as
// the opaque ttContext, and to DeleteTt on teardown.
//
// This milestone's control and bulk splits only need the TT's identity (its
// hub address and port, which the library already passes to the pipe too).
// The per-microframe periodic bandwidth scheduler used by interrupt splits is
// added in a later step; its reservation state will live here.
//
#ifndef __DWCTT_H__
#define __DWCTT_H__

#include <windows.h>

class CDwcTt
{
public:
    CDwcTt(UCHAR bHubAddress, UCHAR bHubPort)
        : m_bHubAddress(bHubAddress)
        , m_bHubPort(bHubPort)
    {
    }

    UCHAR HubAddress(void) const { return m_bHubAddress; }
    UCHAR HubPort(void) const    { return m_bHubPort; }

private:
    const UCHAR m_bHubAddress;
    const UCHAR m_bHubPort;
};

#endif // __DWCTT_H__
