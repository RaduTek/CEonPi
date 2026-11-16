//
// dwclog.h
//
// Debug-zone logging for the DWC2 USB host controller driver.
//
// All four source files share these macros. Errors and warnings print by
// default; the verbose init/register-dump and per-event info zones are off,
// so a normal boot does not pay KITL's ~30 ms per line. Turn a zone on with
// the kernel debug-zone editor (the DBGPARAM is named "DWCHCI"), or change
// DWC_DBG_ZONES_INITIAL in system.cpp.
//
// In a retail (ship) build DEBUGMSG compiles to nothing, so these cost
// nothing and reference no globals.
//
#ifndef __DWCLOG_H__
#define __DWCLOG_H__

#include <windows.h>

// Zone bit numbers (index into the DBGPARAM zone-name table in system.cpp).
#define ZONEID_ERROR    0       // real failures
#define ZONEID_WARN     1       // transfer failures, refused/unreachable devices
#define ZONEID_INIT     2       // register dump, host-init and reset progress
#define ZONEID_INFO     3       // per-event: enumeration, pipe "alive", diagnostics

#define DWC_DBG_ZONES_INITIAL   ((1 << ZONEID_ERROR) | (1 << ZONEID_WARN))

#define DWCERR(x)       DEBUGMSG(DEBUGZONE(ZONEID_ERROR), x)
#define DWCWARN(x)      DEBUGMSG(DEBUGZONE(ZONEID_WARN), x)
#define DWCINIT(x)      DEBUGMSG(DEBUGZONE(ZONEID_INIT), x)
#define DWCINFO(x)      DEBUGMSG(DEBUGZONE(ZONEID_INFO), x)

#ifdef DEBUG
extern DBGPARAM dpCurSettings;  // defined in system.cpp
#endif

#endif // __DWCLOG_H__
