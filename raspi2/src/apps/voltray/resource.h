//
// resource.h - resource IDs for voltray.exe (src\apps\voltray)
//
#ifndef __VOLTRAY_RESOURCE_H
#define __VOLTRAY_RESOURCE_H

#ifndef IDC_STATIC
#define IDC_STATIC      (-1)
#endif

// Trackbar styles (commctrl.h), for the resource compiler, which does not
// include commctrl.h.
#ifndef TBS_AUTOTICKS
#define TBS_AUTOTICKS   0x0001
#endif
#ifndef TBS_VERT
#define TBS_VERT        0x0002
#endif
#ifndef TBS_BOTH
#define TBS_BOTH        0x0008
#endif

#define IDI_VOLTRAY     101             // application icon (first icon resource)
#define IDI_TRAY        102             // tray icon, sound on
#define IDI_TRAY_MUTE   103             // tray icon, muted
#define IDD_VOLUME      104             // the volume popup
#define IDM_TRAY        105             // tray right-click menu

#define IDC_SLIDER      1001
#define IDC_MUTE        1002

#define IDM_EXIT        2001
#define IDM_SETTINGS    2002

#endif // __VOLTRAY_RESOURCE_H
