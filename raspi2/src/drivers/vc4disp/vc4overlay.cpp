//
// vc4overlay.cpp - VideoCore HVS overlay plane for vc4disp (CEonPi)
//
// Video acceleration, milestone V0: a probe. The firmware's "Set plane"
// property tag (0x00048015, used by the Linux fkms driver) puts an image on
// an HVS plane, scaled and colour-converted by the hardware, without VCHIQ.
// Unverified: whether the firmware accepts it while the legacy mailbox
// framebuffer is active (we are not in fkms mode). The probe answers that.
//
// Registry OverlayProbe=1 starts a thread after the first SetMode that:
//   1. queries and logs the number of displays, the id of display 0 and the
//      legacy framebuffer's layer;
//   2. builds a 320x240 YUV 4:2:0 planar test image (colour bars over a
//      luma ramp) in uncached physically contiguous memory;
//   3. stage 1 (t=10 s): shows it on plane 1 of the queried display id,
//      one layer above the framebuffer, at 100,100 scaled to 640x480;
//      stage 2 (t=20 s): moves it to 400,200 scaled to 960x540;
//      stage 3 (t=30 s): disables it;
//      stage 4 (t=40 s, only if the display id is not 0): shows it again on
//      display 0 at 100,100 640x480; stage 5 (t=50 s): disables it.
// Each stage logs the tag's response word, so the log plus what was seen
// on screen tells which display number and layer work.
//
// Set plane message (Linux vc4_firmware_kms.c struct set_plane, 60 bytes,
// little endian; built word by word here to avoid packing questions):
//   w0  display | plane_id << 8 | vc_image_type << 16 | (u8)layer << 24
//   w1  width | height << 16
//   w2  pitch (bytes, luma) | vpitch << 16 (0)
//   w3..w6  src_x, src_y, src_w, src_h (16.16 fixed point)
//   w7  (u16)dst_x | (u16)dst_y << 16
//   w8  dst_w | dst_h << 16
//   w9  alpha | num_planes << 8 | is_vu << 16 | color_encoding << 24
//   w10..w13  bus address of each plane (Y, U, V, 0)
//   w14 transform (0)
// Disabling a plane: same tag, only display and plane_id set.
// The firmware assumes chroma pitch = luma pitch / 2 for 4:2:0.
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

#define TAG_GET_NUM_DISPLAYS    0x00040013
#define TAG_GET_DISPLAY_ID      0x00040016
#define TAG_GET_LAYER           0x0004000C
#define TAG_SET_PLANE           0x00048015
#define TAG_RESPONSE_OK         0x80000000

#define SET_PLANE_WORDS         15

// vc_image_type values (Linux vc_image_types.h)
#define VC_IMAGE_YUV420         3

// color_encoding: VC_IMAGE_YUVINFO_CSC_ITUR_BT601
#define VC_CSC_BT601            1

#define BUS_UNCACHED_ALIAS      0xC0000000

// Test image: 4:2:0 planar, luma pitch a multiple of 32 bytes
#define PROBE_W                 320
#define PROBE_H                 240
#define PROBE_Y_BYTES           (PROBE_W * PROBE_H)
#define PROBE_C_BYTES           ((PROBE_W / 2) * (PROBE_H / 2))
#define PROBE_BYTES             (PROBE_Y_BYTES + 2 * PROBE_C_BYTES)

#define PROBE_PLANE_ID          1
#define PROBE_STAGE_MS          10000

//------------------------------------------------------------------------------
//
//  Function:  ProbeTag
//
//  Sends one tag with a one-word value. Returns TRUE when the IOCTL worked;
//  *pResp gets the tag's request/response word and *pValue the value word.
//
static BOOL ProbeTag(DWORD dwTag, DWORD dwIn, DWORD *pResp, DWORD *pValue)
{
    DWORD msg[4], cbOut = 0;

    msg[0] = dwTag;
    msg[1] = sizeof(DWORD);
    msg[2] = 0;
    msg[3] = dwIn;
    *pResp = 0;
    *pValue = 0;
    if (!KernelIoControl(
        IOCTL_HAL_MY_MAILBOX_PROPERTY, msg, sizeof(msg), msg, sizeof(msg), &cbOut
    )) {
        return FALSE;
    }
    *pResp = msg[2];
    *pValue = msg[3];
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Function:  ProbeSetPlane
//
//  Shows (fShow) or disables the test plane. Logs the result under pszStage.
//
static VOID ProbeSetPlane(LPCWSTR pszStage, BOOL fShow, DWORD dwDisplay,
    int layer, DWORD dwBus, int x, int y, int w, int h)
{
    DWORD msg[3 + SET_PLANE_WORDS], cbOut = 0;
    BOOL fOk;

    memset(msg, 0, sizeof(msg));
    msg[0] = TAG_SET_PLANE;
    msg[1] = SET_PLANE_WORDS * sizeof(DWORD);
    msg[2] = 0;
    msg[3] = (dwDisplay & 0xFF) | (PROBE_PLANE_ID << 8);
    if (fShow) {
        msg[3] |= (VC_IMAGE_YUV420 << 16) | (((DWORD)layer & 0xFF) << 24);
        msg[4] = PROBE_W | (PROBE_H << 16);
        msg[5] = PROBE_W;                       // luma pitch, vpitch 0
        msg[6] = 0;                             // src_x
        msg[7] = 0;                             // src_y
        msg[8] = PROBE_W << 16;                 // src_w
        msg[9] = PROBE_H << 16;                 // src_h
        msg[10] = ((DWORD)x & 0xFFFF) | (((DWORD)y & 0xFFFF) << 16);
        msg[11] = ((DWORD)w & 0xFFFF) | (((DWORD)h & 0xFFFF) << 16);
        msg[12] = 255 | (3 << 8) | (0 << 16) | (VC_CSC_BT601 << 24);
        msg[13] = dwBus;                                    // Y
        msg[14] = dwBus + PROBE_Y_BYTES;                    // U
        msg[15] = dwBus + PROBE_Y_BYTES + PROBE_C_BYTES;    // V
        msg[16] = 0;
        msg[17] = 0;                            // transform
    }

    fOk = KernelIoControl(
        IOCTL_HAL_MY_MAILBOX_PROPERTY, msg, sizeof(msg), msg, sizeof(msg), &cbOut
    );
    if (fShow) {
        RETAILMSG(1, (L"vc4disp: overlay probe %s: show display %u layer %d "
            L"at %d,%d %dx%d: ioctl %s, tag resp 0x%08x, value 0x%08x\r\n",
            pszStage, dwDisplay, layer, x, y, w, h,
            fOk ? L"ok" : L"FAILED", msg[2], msg[3]
        ));
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe %s: disable display %u: "
            L"ioctl %s, tag resp 0x%08x, value 0x%08x\r\n",
            pszStage, dwDisplay, fOk ? L"ok" : L"FAILED", msg[2], msg[3]
        ));
    }
}

//------------------------------------------------------------------------------
//
//  Function:  ProbeFillImage
//
//  Colour bars (BT.601 limited range) in the top three quarters, a luma ramp
//  in the bottom quarter. Bars left to right: white, yellow, cyan, green,
//  magenta, red, blue, black. A U/V mix-up shows as wrong bar colours.
//
static VOID ProbeFillImage(BYTE *pImage)
{
    static const BYTE bars[8][3] = {
        { 235, 128, 128 },  // white
        { 210,  16, 146 },  // yellow
        { 170, 166,  16 },  // cyan
        { 145,  54,  34 },  // green
        { 106, 202, 222 },  // magenta
        {  81,  90, 240 },  // red
        {  41, 240, 110 },  // blue
        {  16, 128, 128 },  // black
    };
    BYTE *pY = pImage;
    BYTE *pU = pImage + PROBE_Y_BYTES;
    BYTE *pV = pU + PROBE_C_BYTES;
    int x, y, bar, rampTop = (PROBE_H * 3) / 4;

    for (y = 0; y < PROBE_H; y++) {
        for (x = 0; x < PROBE_W; x++) {
            if (y < rampTop) {
                bar = (x * 8) / PROBE_W;
                pY[y * PROBE_W + x] = bars[bar][0];
            } else {
                pY[y * PROBE_W + x] = (BYTE)(16 + (x * 219) / (PROBE_W - 1));
            }
        }
    }
    for (y = 0; y < PROBE_H / 2; y++) {
        for (x = 0; x < PROBE_W / 2; x++) {
            if (y * 2 < rampTop) {
                bar = (x * 2 * 8) / PROBE_W;
                pU[y * (PROBE_W / 2) + x] = bars[bar][1];
                pV[y * (PROBE_W / 2) + x] = bars[bar][2];
            } else {
                pU[y * (PROBE_W / 2) + x] = 128;
                pV[y * (PROBE_W / 2) + x] = 128;
            }
        }
    }
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayProbe
//
VOID Vc4GPE::OverlayProbe()
{
    DWORD dwResp, dwValue, dwDisplays = 0, dwDisplayId = 0, dwBus, pa = 0;
    int fbLayer = 0, layer;
    BOOL fHaveId = FALSE;
    BYTE *pImage;

    // Let the desktop come up first
    Sleep(PROBE_STAGE_MS);

    if (ProbeTag(TAG_GET_NUM_DISPLAYS, 0, &dwResp, &dwValue)) {
        dwDisplays = dwValue;
        RETAILMSG(1, (L"vc4disp: overlay probe: get num displays: tag resp "
            L"0x%08x, displays %u\r\n", dwResp, dwValue));
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe: get num displays: ioctl FAILED\r\n"));
    }
    if (ProbeTag(TAG_GET_DISPLAY_ID, 0, &dwResp, &dwValue)) {
        RETAILMSG(1, (L"vc4disp: overlay probe: get display id (index 0): tag "
            L"resp 0x%08x, id %u\r\n", dwResp, dwValue));
        if ((dwResp & TAG_RESPONSE_OK) != 0) {
            dwDisplayId = dwValue;
            fHaveId = TRUE;
        }
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe: get display id: ioctl FAILED\r\n"));
    }
    if (ProbeTag(TAG_GET_LAYER, 0, &dwResp, &dwValue)) {
        RETAILMSG(1, (L"vc4disp: overlay probe: get fb layer: tag resp 0x%08x, "
            L"layer %d\r\n", dwResp, (int)dwValue));
        if ((dwResp & TAG_RESPONSE_OK) != 0) fbLayer = (int)dwValue;
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe: get fb layer: ioctl FAILED\r\n"));
    }

    // One layer above the framebuffer (s8 in the message)
    layer = fbLayer + 1;
    if (layer > 127) layer = 127;
    if (layer < -126) layer = -126;

    pImage = (BYTE*)AllocPhysMem(
        PROBE_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa
    );
    if (pImage == NULL) {
        RETAILMSG(1, (L"vc4disp: overlay probe: image allocation failed\r\n"));
        return;
    }
    dwBus = BUS_UNCACHED_ALIAS | pa;
    ProbeFillImage(pImage);
    RETAILMSG(1, (L"vc4disp: overlay probe: %ux%u YUV420 image at bus 0x%08x, "
        L"displays %u, display id %u%s, layer %d\r\n",
        PROBE_W, PROBE_H, dwBus, dwDisplays, dwDisplayId,
        fHaveId ? L"" : L" (not answered, using 0)", layer));

    ProbeSetPlane(L"stage 1", TRUE, dwDisplayId, layer, dwBus, 100, 100, 640, 480);
    Sleep(PROBE_STAGE_MS);
    ProbeSetPlane(L"stage 2", TRUE, dwDisplayId, layer, dwBus, 400, 200, 960, 540);
    Sleep(PROBE_STAGE_MS);
    ProbeSetPlane(L"stage 3", FALSE, dwDisplayId, layer, dwBus, 0, 0, 0, 0);

    if (dwDisplayId != 0) {
        Sleep(PROBE_STAGE_MS);
        ProbeSetPlane(L"stage 4", TRUE, 0, layer, dwBus, 100, 100, 640, 480);
        Sleep(PROBE_STAGE_MS);
        ProbeSetPlane(L"stage 5", FALSE, 0, layer, dwBus, 0, 0, 0, 0);
    }

    // The firmware may still scan the image if a disable did not take, so
    // the buffer is deliberately not freed (115 KB, probe builds only)
    RETAILMSG(1, (L"vc4disp: overlay probe done\r\n"));
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayProbeThreadProc
//
DWORD WINAPI Vc4GPE::OverlayProbeThreadProc(LPVOID pContext)
{
    ((Vc4GPE*)pContext)->OverlayProbe();
    return 0;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayProbeStart
//
//  Called from SetMode; starts the probe thread once.
//
VOID Vc4GPE::OverlayProbeStart()
{
    HANDLE hThread;

    if (!m_fOverlayProbe || m_fOverlayProbeStarted) return;
    m_fOverlayProbeStarted = TRUE;

    hThread = CreateThread(NULL, 0, OverlayProbeThreadProc, this, 0, NULL);
    if (hThread == NULL) {
        RETAILMSG(1, (L"vc4disp: overlay probe thread creation failed\r\n"));
        return;
    }
    CloseHandle(hThread);
    RETAILMSG(1, (L"vc4disp: overlay probe starts in %u s\r\n",
        PROBE_STAGE_MS / 1000));
}
