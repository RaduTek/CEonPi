//
// vc4ovl.h - vc4disp video overlay escape interface (CEonPi)
//
// The display driver vc4disp can show one YUV image on a VideoCore HVS
// plane above the desktop, scaled and colour-converted by the hardware
// (firmware "Set plane" property tag). Until the DirectDraw overlay exists,
// applications reach it through ExtEscape(hdc, VC4OVL_ESCAPE, ...) with a
// VC4OVL_REQUEST in and a VC4OVL_INFO out. ExtEscape returns > 0 on success,
// < 0 on failure, 0 if the driver does not know the escape.
//
//   CREATE   dwFourCC (YV12 or I420), dwWidth, dwHeight (both even),
//            dwBuffers (1..VC4OVL_MAX_BUFFERS):
//            allocates the frame buffers (cleared to black) and maps them
//            into the calling process (uncached); hidden until SHOW.
//            One overlay at a time; fails with ERROR_BUSY while another live
//            process owns it.
//   DESTROY  hides the plane, unmaps and frees the buffers.
//   SHOW     rcSrc (pixels of the image), rcDst (screen pixels; clipped to
//            the screen by the driver), fShow.
//   FLIP     dwBuffer: the buffer to display from now on. With
//            VC4OVL_FLAG_WAIT the call returns once the firmware has
//            answered the plane update (dwSeqDone >= the flip's sequence).
//   QUERY    only fills VC4OVL_INFO.
//
// Buffer layout (VC4OVL_INFO): frame i starts at pFrame[i]; plane p of a
// frame starts at dwOffset[p] in memory order (YV12: Y, V, U; I420: Y, U,
// V). dwPitch is the luma pitch, dwChromaPitch the pitch of the two 4:2:0
// chroma planes. Planar 4:2:0 only: packed YUY2/UYVY planes were accepted
// by the firmware but never shown (Pi 1 Model B, 2026-11-25).
//
// Every SHOW and FLIP takes a sequence number (dwSeqQueued); dwSeqDone is
// the newest one the firmware has answered. Whether the HVS has stopped
// reading the previous buffer at that point (next vsync?) is not known yet:
// render into a buffer that left the screen two flips ago (3 buffers) to
// be safe.
//
#ifndef __VC4OVL_H
#define __VC4OVL_H

#define VC4OVL_ESCAPE               0x00086F00

#define VC4OVL_CMD_CREATE           1
#define VC4OVL_CMD_DESTROY          2
#define VC4OVL_CMD_SHOW             3
#define VC4OVL_CMD_FLIP             4
#define VC4OVL_CMD_QUERY            5

#define VC4OVL_FLAG_WAIT            0x00000001

#define VC4OVL_MAX_BUFFERS          3
#define VC4OVL_MAX_WIDTH            2048
#define VC4OVL_MAX_HEIGHT           2048

#define VC4OVL_FOURCC(a, b, c, d) \
    ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | \
     ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))

#define VC4OVL_FOURCC_YV12          VC4OVL_FOURCC('Y', 'V', '1', '2')
#define VC4OVL_FOURCC_I420          VC4OVL_FOURCC('I', '4', '2', '0')

typedef struct {
    DWORD dwSize;           // sizeof(VC4OVL_REQUEST)
    DWORD dwCommand;        // VC4OVL_CMD_xxx
    DWORD dwFlags;          // VC4OVL_FLAG_xxx
    DWORD dwFourCC;         // CREATE
    DWORD dwWidth;          // CREATE (even)
    DWORD dwHeight;         // CREATE (even for 4:2:0)
    DWORD dwBuffers;        // CREATE
    RECT  rcSrc;            // SHOW
    RECT  rcDst;            // SHOW
    DWORD fShow;            // SHOW
    DWORD dwBuffer;         // FLIP
} VC4OVL_REQUEST;

typedef struct {
    DWORD dwSize;           // sizeof(VC4OVL_INFO)
    DWORD dwFourCC;
    DWORD dwWidth;
    DWORD dwHeight;
    DWORD dwBuffers;        // 0 = no overlay
    DWORD dwPitch;
    DWORD dwChromaPitch;
    DWORD dwOffset[3];
    DWORD dwFrameBytes;
    BYTE *pFrame[VC4OVL_MAX_BUFFERS];   // CREATE only (caller's mapping)
    DWORD dwShown;          // buffer index on screen (last queued)
    DWORD dwSeqQueued;
    DWORD dwSeqDone;
    DWORD dwScreenWidth;
    DWORD dwScreenHeight;
} VC4OVL_INFO;

#endif
