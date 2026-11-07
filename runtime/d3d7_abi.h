/*
 * d3d7_abi.h - the DirectDraw7 / Direct3D7 ABI as *linked against* by NFS5's
 * renderer plugin Drivers/dx7z.dll, plus the guest-memory structures it passes.
 *
 * Source of truth: the public DirectX 7 SDK headers as redistributed by Wine
 * (LGPL): https://raw.githubusercontent.com/wine-mirror/wine/master/include/
 *   ddraw.h    (IDirectDraw7, IDirectDrawSurface7, DDSURFACEDESC2, DDSCAPS2,
 *               DDPIXELFORMAT, DDCOLORKEY, DDSCAPS_*, DDPF_*, DDBD_*)
 *   d3d.h      (IDirect3D7, IDirect3DDevice7, IDirect3DTexture2,
 *               IDirect3DViewport3, IDirect3DVertexBuffer7, IDirect3DMaterial3,
 *               IDirect3DLight, IDirect3DExecuteBuffer)
 *   d3dtypes.h (D3DTLVERTEX/D3DLVERTEX/D3DVERTEX, D3DMATRIX, D3DVIEWPORT7,
 *               D3DMATERIAL7, D3DLIGHT7, D3DFVF_*, all D3D7 enums)
 *   d3dcaps.h  (D3DDEVICEDESC7, D3DPRIMCAPS and every capability bit below)
 *
 * Wine's ddraw.h/d3d.h carry the original Microsoft vtable order in the
 * /*0c*\/-style byte-offset comments; the slot numbers below are those
 * offsets divided by 4.  A COM method is stdcall with `this` as argument 0, so
 * inside a win32hle shim `A32(0)` is `this`, `A32(1)` the first real argument.
 * Everything here is i386-safe: 4-byte packing (no 8-byte members), so the
 * natural gcc layout equals the Windows __i386__ pack(4) layout.
 *
 * This header is private to the renderer; guest-visible values are only ever
 * written through MEM32/MEM16/MEM8 at guest VAs.
 */
#ifndef NFS5_D3D7_ABI_H
#define NFS5_D3D7_ABI_H

#include <stdint.h>

/* ---------------------------------------------------------------- GUIDs ---
 * Byte order is the in-memory GUID: Data1 little-endian, Data2/Data3
 * little-endian, Data4 as bytes.  dx7z keeps exactly these 16-byte blobs in
 * its .rdata at 0x10016A60..0x10016B10 (verified by scanning the DLL); the
 * ones marked "seen" are referenced by the lifted code. */
typedef struct { uint8_t b[16]; } D3D7Guid;

#define D3D7_GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                     \
    { { (uint8_t)((a) & 0xFF), (uint8_t)(((a) >> 8) & 0xFF),                    \
        (uint8_t)(((a) >> 16) & 0xFF), (uint8_t)(((a) >> 24) & 0xFF),           \
        (uint8_t)((b) & 0xFF), (uint8_t)(((b) >> 8) & 0xFF),                    \
        (uint8_t)((c) & 0xFF), (uint8_t)(((c) >> 8) & 0xFF),                    \
        (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) } }

static const uint8_t D3D7_IID_IDirectDraw7[16] =
    { 0xc0,0x5e,0xe6,0x15, 0x9c,0x3b, 0xd2,0x11, 0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b }; /* seen */
static const uint8_t D3D7_IID_IDirectDraw4[16] =
    { 0x9a,0x50,0x59,0x9c, 0xbd,0x39, 0xd1,0x11, 0x8c,0x4a,0x00,0xc0,0x4f,0xd9,0x30,0xc5 }; /* seen (fallback) */
static const uint8_t D3D7_IID_IDirectDrawSurface7[16] =
    { 0x80,0x5a,0x67,0x06, 0x9b,0x3b, 0xd2,0x11, 0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b }; /* seen */
static const uint8_t D3D7_IID_IDirect3D7[16] =
    { 0x77,0x9e,0x04,0xf5, 0x61,0x48, 0xd2,0x11, 0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8 }; /* seen */
static const uint8_t D3D7_IID_IDirect3D3[16] =
    { 0x40,0x32,0x22,0xbb, 0x2b,0xe7, 0xd0,0x11, 0xa9,0xb4,0x00,0xaa,0x00,0xc0,0x99,0x3e }; /* seen (fallback) */
static const uint8_t D3D7_IID_IDirect3DDevice7[16] =
    { 0x79,0x9e,0x04,0xf5, 0x61,0x48, 0xd2,0x11, 0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8 };
static const uint8_t D3D7_IID_IDirect3DTexture2[16] =
    { 0x02,0x15,0x28,0x93, 0xf8,0x8c, 0xd0,0x11, 0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29 };
static const uint8_t D3D7_IID_IDirectDrawGammaControl[16] =
    { 0x3e,0x1c,0xc1,0x69, 0x6b,0xb4, 0xd1,0x11, 0xad,0x7a,0x00,0xc0,0x4f,0xc2,0x9b,0x4e }; /* seen (optional) */
static const uint8_t D3D7_IID_IUnknown[16] =
    { 0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00, 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };

/* Device class CLSIDs (IDirect3D7::CreateDevice takes one of these, not an
 * IID - which is why the DLL has no IID_IDirect3DDevice7). */
static const uint8_t D3D7_CLSID_IDirect3DHALDevice[16] =
    { 0xe0,0x3d,0xe6,0x84, 0xaa,0x46, 0xcf,0x11, 0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e }; /* seen */
static const uint8_t D3D7_CLSID_IDirect3DTnLHalDevice[16] =
    { 0x78,0x9e,0x04,0xf5, 0x61,0x48, 0xd2,0x11, 0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8 }; /* seen */
static const uint8_t D3D7_CLSID_IDirect3DRGBDevice[16] =
    { 0x60,0x5c,0x66,0xa4, 0x73,0x26, 0xcf,0x11, 0xa3,0x1a,0x00,0xaa,0x00,0xb9,0x33,0x56 }; /* seen */
static const uint8_t D3D7_CLSID_IDirect3DRampDevice[16] =
    { 0x20,0x6b,0x08,0xf2, 0x9f,0x25, 0xcf,0x11, 0xa3,0x1a,0x00,0xaa,0x00,0xb9,0x33,0x56 }; /* seen */
static const uint8_t D3D7_CLSID_IDirect3DMMXDevice[16] =
    { 0xa1,0x49,0x19,0x88, 0xf3,0xd6, 0xd0,0x11, 0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29 }; /* seen */
static const uint8_t D3D7_CLSID_IDirect3DRefDevice[16] =
    { 0x43,0x66,0x93,0x50, 0xe9,0x13, 0xd1,0x11, 0x89,0xaa,0x00,0xa0,0xc9,0x05,0x41,0x29 }; /* seen */

static inline int d3d7_guid_eq(const void *a, const void *b) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static inline int d3d7_guid_is_zero(const void *a) {
    const uint8_t *x = (const uint8_t *)a;
    for (int i = 0; i < 16; i++) if (x[i]) return 0;
    return 1;
}

/* ------------------------------------------------------- vtable slots ----
 * Method order is the public DX7 ABI (Wine header offsets in the comments).
 * Slot 0..2 are always IUnknown QueryInterface/AddRef/Release. */
enum {
    /* IDirect3D7: 5 methods (d3d.h IDirect3D7) */
    D3D7_D3D_EnumDevices          = 3,   /* +0x0c */
    D3D7_D3D_CreateDevice         = 4,   /* +0x10 (REFCLSID class, IDirectDrawSurface7*, IDirect3DDevice7**) */
    D3D7_D3D_CreateVertexBuffer   = 5,   /* +0x14 */
    D3D7_D3D_EnumZBufferFormats   = 6,   /* +0x18 */
    D3D7_D3D_EvictManagedTextures = 7,   /* +0x1c */
    D3D7_D3D_METHODS              = 8
};

enum {
    /* IDirect3DDevice7: 46 methods (d3d.h IDirect3DDevice7) */
    D3D7_DEV_GetCaps                  = 3,   /* +0x0c */
    D3D7_DEV_EnumTextureFormats       = 4,   /* +0x10 */
    D3D7_DEV_BeginScene               = 5,   /* +0x14 */
    D3D7_DEV_EndScene                 = 6,   /* +0x18 */
    D3D7_DEV_GetDirect3D              = 7,   /* +0x1c */
    D3D7_DEV_SetRenderTarget          = 8,   /* +0x20 */
    D3D7_DEV_GetRenderTarget          = 9,   /* +0x24 */
    D3D7_DEV_Clear                    = 10,  /* +0x28 */
    D3D7_DEV_SetTransform             = 11,  /* +0x2c */
    D3D7_DEV_GetTransform             = 12,  /* +0x30 */
    D3D7_DEV_SetViewport              = 13,  /* +0x34 */
    D3D7_DEV_MultiplyTransform        = 14,  /* +0x38 */
    D3D7_DEV_GetViewport              = 15,  /* +0x3c */
    D3D7_DEV_SetMaterial              = 16,  /* +0x40 */
    D3D7_DEV_GetMaterial              = 17,  /* +0x44 */
    D3D7_DEV_SetLight                 = 18,  /* +0x48 */
    D3D7_DEV_GetLight                 = 19,  /* +0x4c */
    D3D7_DEV_SetRenderState           = 20,  /* +0x50 */
    D3D7_DEV_GetRenderState           = 21,  /* +0x54 */
    D3D7_DEV_BeginStateBlock          = 22,  /* +0x58 */
    D3D7_DEV_EndStateBlock            = 23,  /* +0x5c */
    D3D7_DEV_PreLoad                  = 24,  /* +0x60 */
    D3D7_DEV_DrawPrimitive            = 25,  /* +0x64 */
    D3D7_DEV_DrawIndexedPrimitive     = 26,  /* +0x68 */
    D3D7_DEV_SetClipStatus            = 27,  /* +0x6c */
    D3D7_DEV_GetClipStatus            = 28,  /* +0x70 */
    D3D7_DEV_DrawPrimitiveStrided     = 29,  /* +0x74 */
    D3D7_DEV_DrawIndexedPrimitiveStrided = 30, /* +0x78 */
    D3D7_DEV_DrawPrimitiveVB          = 31,  /* +0x7c */
    D3D7_DEV_DrawIndexedPrimitiveVB   = 32,  /* +0x80 */
    D3D7_DEV_ComputeSphereVisibility  = 33,  /* +0x84 */
    D3D7_DEV_GetTexture               = 34,  /* +0x88 */
    D3D7_DEV_SetTexture               = 35,  /* +0x8c */
    D3D7_DEV_GetTextureStageState     = 36,  /* +0x90 */
    D3D7_DEV_SetTextureStageState     = 37,  /* +0x94 */
    D3D7_DEV_ValidateDevice           = 38,  /* +0x98 */
    D3D7_DEV_ApplyStateBlock          = 39,  /* +0x9c */
    D3D7_DEV_CaptureStateBlock        = 40,  /* +0xa0 */
    D3D7_DEV_DeleteStateBlock         = 41,  /* +0xa4 */
    D3D7_DEV_CreateStateBlock         = 42,  /* +0xa8 */
    D3D7_DEV_Load                     = 43,  /* +0xac */
    D3D7_DEV_LightEnable              = 44,  /* +0xb0 */
    D3D7_DEV_GetLightEnable           = 45,  /* +0xb4 */
    D3D7_DEV_SetClipPlane             = 46,  /* +0xb8 */
    D3D7_DEV_GetClipPlane             = 47,  /* +0xbc */
    D3D7_DEV_GetInfo                  = 48,  /* +0xc0 */
    D3D7_DEV_METHODS                  = 49
};

enum {
    /* IDirectDraw7: 27 methods (ddraw.h IDirectDraw7) */
    DD7_Compact               = 3,   /* +0x0c */
    DD7_CreateClipper         = 4,   /* +0x10 */
    DD7_CreatePalette         = 5,   /* +0x14 */
    DD7_CreateSurface         = 6,   /* +0x18 */
    DD7_DuplicateSurface      = 7,   /* +0x1c */
    DD7_EnumDisplayModes      = 8,   /* +0x20 */
    DD7_EnumSurfaces          = 9,   /* +0x24 */
    DD7_FlipToGDISurface      = 10,  /* +0x28 */
    DD7_GetCaps               = 11,  /* +0x2c */
    DD7_GetDisplayMode        = 12,  /* +0x30 */
    DD7_GetFourCCCodes        = 13,  /* +0x34 */
    DD7_GetGDISurface         = 14,  /* +0x38 */
    DD7_GetMonitorFrequency   = 15,  /* +0x3c */
    DD7_GetScanLine           = 16,  /* +0x40 */
    DD7_GetVerticalBlankStatus= 17,  /* +0x44 */
    DD7_Initialize            = 18,  /* +0x48 */
    DD7_RestoreDisplayMode    = 19,  /* +0x4c */
    DD7_SetCooperativeLevel   = 20,  /* +0x50 */
    DD7_SetDisplayMode        = 21,  /* +0x54 */
    DD7_WaitForVerticalBlank  = 22,  /* +0x58 */
    DD7_GetAvailableVidMem    = 23,  /* +0x5c */
    DD7_GetSurfaceFromDC      = 24,  /* +0x60 */
    DD7_RestoreAllSurfaces    = 25,  /* +0x64 */
    DD7_TestCooperativeLevel  = 26,  /* +0x68 */
    DD7_GetDeviceIdentifier   = 27,  /* +0x6c */
    DD7_StartModeTest         = 28,  /* +0x70 */
    DD7_EvaluateMode          = 29,  /* +0x74 */
    DD7_METHODS               = 30
};

enum {
    /* IDirectDrawSurface7: 46 methods (ddraw.h); slots 0..32 are the
     * IDirectDrawSurface/2/3 prefix, 33..45 are the 7-only tail. */
    DDS7_AddAttachedSurface    = 3,   /* +0x0c */
    DDS7_AddOverlayDirtyRect   = 4,
    DDS7_Blt                   = 5,   /* +0x14 */
    DDS7_BltBatch              = 6,
    DDS7_BltFast               = 7,   /* +0x1c */
    DDS7_DeleteAttachedSurface = 8,
    DDS7_EnumAttachedSurfaces  = 9,
    DDS7_EnumOverlayZOrders    = 10,
    DDS7_Flip                  = 11,  /* +0x2c */
    DDS7_GetAttachedSurface    = 12,  /* +0x30 */
    DDS7_GetBltStatus          = 13,
    DDS7_GetCaps               = 14,
    DDS7_GetClipper            = 15,
    DDS7_GetColorKey           = 16,  /* +0x40 */
    DDS7_GetDC                 = 17,
    DDS7_GetFlipStatus         = 18,
    DDS7_GetOverlayPosition    = 19,
    DDS7_GetPalette            = 20,  /* +0x50 */
    DDS7_GetPixelFormat        = 21,
    DDS7_GetSurfaceDesc        = 22,  /* +0x58 */
    DDS7_Initialize            = 23,
    DDS7_IsLost                = 24,
    DDS7_Lock                  = 25,  /* +0x64 */
    DDS7_ReleaseDC             = 26,
    DDS7_Restore               = 27,
    DDS7_SetClipper            = 28,
    DDS7_SetColorKey           = 29,  /* +0x74 */
    DDS7_SetOverlayPosition    = 30,
    DDS7_SetPalette            = 31,
    DDS7_Unlock                = 32,  /* +0x80 */
    DDS7_UpdateOverlay         = 33,
    DDS7_UpdateOverlayDisplay  = 34,
    DDS7_UpdateOverlayZOrder   = 35,
    DDS7_GetDDInterface        = 36,  /* +0x90 */
    DDS7_PageLock              = 37,
    DDS7_PageUnlock            = 38,
    DDS7_SetSurfaceDesc        = 39,  /* +0x9c */
    DDS7_SetPrivateData        = 40,  /* +0xa0 */
    DDS7_GetPrivateData        = 41,
    DDS7_FreePrivateData       = 42,
    DDS7_GetUniquenessValue    = 43,
    DDS7_ChangeUniquenessValue = 44,
    DDS7_SetPriority           = 45,
    DDS7_GetPriority           = 46,  /* +0xb8 */
    DDS7_SetLOD                = 47,
    DDS7_GetLOD                = 48,
    DDS7_METHODS               = 49
};

/* -------------------------------------------------------- D3D7 enums ---- */
enum {
    D3D7_PT_POINTLIST = 1, D3D7_PT_LINELIST = 2, D3D7_PT_LINESTRIP = 3,
    D3D7_PT_TRIANGLELIST = 4, D3D7_PT_TRIANGLESTRIP = 5, D3D7_PT_TRIANGLEFAN = 6
};

/* D3D7 FVF (d3dtypes.h). NOTE: differs from D3D8 - RESERVED1 (0x20) means one
 * DWORD of padding after the position (D3DLVERTEX's dwReserved). */
#define D3D7_FVF_POSITION_MASK   0x00e
#define D3D7_FVF_XYZ             0x002
#define D3D7_FVF_XYZRHW          0x004
#define D3D7_FVF_XYZB1           0x006
#define D3D7_FVF_NORMAL          0x010
#define D3D7_FVF_RESERVED1       0x020
#define D3D7_FVF_DIFFUSE         0x040
#define D3D7_FVF_SPECULAR        0x080
#define D3D7_FVF_TEXCOUNT_SHIFT  8
#define D3D7_FVF_TEX1            0x100
#define D3D7_FVF_TEX2            0x200
#define D3D7_FVF_TEXCOORD_SIZE(fvf, i) (1u << ((((fvf) >> (16 + 2 * (i))) & 3) * 2))
/* D3D7 texture coord size code: 0 -> 2 floats, 1 -> 3, 2 -> 4, 3 -> 1 */

enum { /* D3DTRANSFORMSTATETYPE */
    D3D7_TS_WORLD = 1, D3D7_TS_VIEW = 2, D3D7_TS_PROJECTION = 3,
    D3D7_TS_WORLD1 = 4, D3D7_TS_WORLD2 = 5, D3D7_TS_WORLD3 = 6,
    D3D7_TS_TEXTURE0 = 16
};

enum { /* D3DRENDERSTATETYPE - D3D7 values (d3dtypes.h) */
    D3D7_RS_TEXTUREHANDLE = 1, D3D7_RS_ANTIALIAS = 2, D3D7_RS_TEXTUREADDRESS = 3,
    D3D7_RS_TEXTUREPERSPECTIVE = 4, D3D7_RS_WRAPU = 5, D3D7_RS_WRAPV = 6,
    D3D7_RS_ZENABLE = 7, D3D7_RS_FILLMODE = 8, D3D7_RS_SHADEMODE = 9,
    D3D7_RS_LINEPATTERN = 10, D3D7_RS_ZWRITEENABLE = 14,
    D3D7_RS_ALPHATESTENABLE = 15, D3D7_RS_LASTPIXEL = 16,
    D3D7_RS_TEXTUREMAG = 17, D3D7_RS_TEXTUREMIN = 18,
    D3D7_RS_SRCBLEND = 19, D3D7_RS_DESTBLEND = 20, D3D7_RS_TEXTUREMAPBLEND = 21,
    D3D7_RS_CULLMODE = 22, D3D7_RS_ZFUNC = 23, D3D7_RS_ALPHAREF = 24,
    D3D7_RS_ALPHAFUNC = 25, D3D7_RS_DITHERENABLE = 26,
    D3D7_RS_ALPHABLENDENABLE = 27, D3D7_RS_FOGENABLE = 28,
    D3D7_RS_SPECULARENABLE = 29, D3D7_RS_ZVISIBLE = 30,
    D3D7_RS_SUBPIXEL = 31, D3D7_RS_SUBPIXELX = 32,
    D3D7_RS_FOGCOLOR = 34, D3D7_RS_FOGTABLEMODE = 35,
    D3D7_RS_FOGTABLESTART = 36, D3D7_RS_FOGTABLEEND = 37,
    D3D7_RS_FOGTABLEDENSITY = 38,
    /* aliases used by the D3D7 SDK: the table fog start/end/density ARE the
     * FOGSTART/FOGEND/FOGDENSITY render states (same values) */
    D3D7_RS_FOGSTART = 36, D3D7_RS_FOGEND = 37, D3D7_RS_FOGDENSITY = 38,
    D3D7_RS_EDGEANTIALIAS = 40, D3D7_RS_COLORKEYENABLE = 41,
    D3D7_RS_BORDERCOLOR = 43, D3D7_RS_TEXTUREADDRESSU = 44,
    D3D7_RS_TEXTUREADDRESSV = 45, D3D7_RS_MIPMAPLODBIAS = 46,
    D3D7_RS_ZBIAS = 47, D3D7_RS_RANGEFOGENABLE = 48,
    D3D7_RS_ANISOTROPY = 49, D3D7_RS_FLUSHBATCH = 50,
    D3D7_RS_STENCILENABLE = 52, D3D7_RS_STENCILFAIL = 53,
    D3D7_RS_STENCILZFAIL = 54, D3D7_RS_STENCILPASS = 55,
    D3D7_RS_STENCILFUNC = 56, D3D7_RS_STENCILREF = 57,
    D3D7_RS_STENCILMASK = 58, D3D7_RS_STENCILWRITEMASK = 59,
    D3D7_RS_TEXTUREFACTOR = 60,
    D3D7_RS_WRAP0 = 128,
    D3D7_RS_CLIPPING = 136, D3D7_RS_LIGHTING = 137, D3D7_RS_EXTENTS = 138,
    D3D7_RS_AMBIENT = 139, D3D7_RS_FOGVERTEXMODE = 140,
    D3D7_RS_COLORVERTEX = 141, D3D7_RS_LOCALVIEWER = 142,
    D3D7_RS_NORMALIZENORMALS = 143, D3D7_RS_COLORKEYBLENDENABLE = 144,
    D3D7_RS_DIFFUSEMATERIALSOURCE = 145, D3D7_RS_SPECULARMATERIALSOURCE = 146,
    D3D7_RS_AMBIENTMATERIALSOURCE = 147, D3D7_RS_EMISSIVEMATERIALSOURCE = 148,
    D3D7_RS_VERTEXBLEND = 151, D3D7_RS_CLIPPLANEENABLE = 152,
    D3D7_RS_COUNT = 256
};

enum { /* D3DTEXTURESTAGESTATETYPE */
    D3D7_TSS_COLOROP = 1, D3D7_TSS_COLORARG1 = 2, D3D7_TSS_COLORARG2 = 3,
    D3D7_TSS_ALPHAOP = 4, D3D7_TSS_ALPHAARG1 = 5, D3D7_TSS_ALPHAARG2 = 6,
    D3D7_TSS_BUMPENVMAT00 = 7, D3D7_TSS_BUMPENVMAT01 = 8,
    D3D7_TSS_BUMPENVMAT10 = 9, D3D7_TSS_BUMPENVMAT11 = 10,
    D3D7_TSS_TEXCOORDINDEX = 11, D3D7_TSS_ADDRESS = 12,
    D3D7_TSS_ADDRESSU = 13, D3D7_TSS_ADDRESSV = 14, D3D7_TSS_BORDERCOLOR = 15,
    D3D7_TSS_MAGFILTER = 16, D3D7_TSS_MINFILTER = 17, D3D7_TSS_MIPFILTER = 18,
    D3D7_TSS_MIPMAPLODBIAS = 19, D3D7_TSS_MAXMIPLEVEL = 20,
    D3D7_TSS_MAXANISOTROPY = 21, D3D7_TSS_BUMPENVLSCALE = 22,
    D3D7_TSS_BUMPENVLOFFSET = 23, D3D7_TSS_TEXTURETRANSFORMFLAGS = 24,
    D3D7_TSS_COUNT = 32
};

enum { /* D3DTEXTUREOP */
    D3D7_TOP_DISABLE = 1, D3D7_TOP_SELECTARG1 = 2, D3D7_TOP_SELECTARG2 = 3,
    D3D7_TOP_MODULATE = 4, D3D7_TOP_MODULATE2X = 5, D3D7_TOP_MODULATE4X = 6,
    D3D7_TOP_ADD = 7, D3D7_TOP_ADDSIGNED = 8, D3D7_TOP_ADDSIGNED2X = 9,
    D3D7_TOP_SUBTRACT = 10, D3D7_TOP_ADDSMOOTH = 11,
    D3D7_TOP_BLENDDIFFUSEALPHA = 12, D3D7_TOP_BLENDTEXTUREALPHA = 13,
    D3D7_TOP_BLENDFACTORALPHA = 14, D3D7_TOP_BLENDTEXTUREALPHAPM = 15,
    D3D7_TOP_BLENDCURRENTALPHA = 16, D3D7_TOP_PREMODULATE = 17,
    D3D7_TOP_MODULATEALPHA_ADDCOLOR = 18, D3D7_TOP_MODULATECOLOR_ADDALPHA = 19,
    D3D7_TOP_MODULATEINVALPHA_ADDCOLOR = 20,
    D3D7_TOP_MODULATEINVCOLOR_ADDALPHA = 21,
    D3D7_TOP_BUMPENVMAP = 22, D3D7_TOP_BUMPENVMAPLUMINANCE = 23,
    D3D7_TOP_DOTPRODUCT3 = 24
};

/* D3DTA_* texture-argument selectors (d3dtypes.h) */
#define D3D7_TA_SELECTMASK      0x0000000fu
#define D3D7_TA_DIFFUSE         0x00000000u
#define D3D7_TA_CURRENT         0x00000001u
#define D3D7_TA_TEXTURE         0x00000002u
#define D3D7_TA_TFACTOR         0x00000003u
#define D3D7_TA_SPECULAR        0x00000004u
#define D3D7_TA_COMPLEMENT      0x00000010u
#define D3D7_TA_ALPHAREPLICATE  0x00000020u

enum { /* D3DTEXTUREADDRESS */
    D3D7_TADDR_WRAP = 1, D3D7_TADDR_MIRROR = 2, D3D7_TADDR_CLAMP = 3,
    D3D7_TADDR_BORDER = 4, D3D7_TADDR_MIRRORONCE = 5
};

enum { /* D3DTEXTUREFILTERTYPE */
    D3D7_TF_NONE = 0, D3D7_TF_POINT = 1, D3D7_TF_LINEAR = 2, D3D7_TF_ANISOTROPIC = 3,
    D3D7_TF_FLATCUBIC = 4, D3D7_TF_GAUSSIANCUBIC = 5
};

enum { /* D3DBLEND */
    D3D7_BLEND_ZERO = 1, D3D7_BLEND_ONE = 2, D3D7_BLEND_SRCCOLOR = 3,
    D3D7_BLEND_INVSRCCOLOR = 4, D3D7_BLEND_SRCALPHA = 5, D3D7_BLEND_INVSRCALPHA = 6,
    D3D7_BLEND_DESTALPHA = 7, D3D7_BLEND_INVDESTALPHA = 8, D3D7_BLEND_DESTCOLOR = 9,
    D3D7_BLEND_INVDESTCOLOR = 10, D3D7_BLEND_SRCALPHASAT = 11,
    D3D7_BLEND_BOTHSRCALPHA = 12, D3D7_BLEND_BOTHINVSRCALPHA = 13
};

enum { /* D3DBLENDOP - D3D7 has no D3DRS_BLENDOP; blend ops are fixed ADD.
        * Kept for the renderer's internal table. */
    D3D7_BLENDOP_ADD = 1, D3D7_BLENDOP_SUBTRACT = 2, D3D7_BLENDOP_REVSUBTRACT = 3,
    D3D7_BLENDOP_MIN = 4, D3D7_BLENDOP_MAX = 5
};

enum { /* D3DCMPFUNC */
    D3D7_CMP_NEVER = 1, D3D7_CMP_LESS = 2, D3D7_CMP_EQUAL = 3, D3D7_CMP_LESSEQUAL = 4,
    D3D7_CMP_GREATER = 5, D3D7_CMP_NOTEQUAL = 6, D3D7_CMP_GREATEREQUAL = 7,
    D3D7_CMP_ALWAYS = 8
};

enum { /* D3DFOGMODE */
    D3D7_FOG_NONE = 0, D3D7_FOG_EXP = 1, D3D7_FOG_EXP2 = 2, D3D7_FOG_LINEAR = 3
};

enum { D3D7_SHADE_FLAT = 1, D3D7_SHADE_GOURAUD = 2, D3D7_SHADE_PHONG = 3 };
enum { D3D7_CULL_NONE = 1, D3D7_CULL_CW = 2, D3D7_CULL_CCW = 3 };
enum { D3D7_FILL_POINT = 1, D3D7_FILL_WIREFRAME = 2, D3D7_FILL_SOLID = 3 };
enum { D3D7_ZB_FALSE = 0, D3D7_ZB_TRUE = 1, D3D7_ZB_USEW = 2 };
enum { D3D7_ZCMP_NEVER = 1, D3D7_ZCMP_LESS = 2, D3D7_ZCMP_EQUAL = 3,
       D3D7_ZCMP_LESSEQUAL = 4, D3D7_ZCMP_GREATER = 5, D3D7_ZCMP_NOTEQUAL = 6,
       D3D7_ZCMP_GREATEREQUAL = 7, D3D7_ZCMP_ALWAYS = 8 };
enum { D3D7_LIGHT_POINT = 1, D3D7_LIGHT_SPOT = 2, D3D7_LIGHT_DIRECTIONAL = 3,
       D3D7_LIGHT_PARALLELPOINT = 4, D3D7_LIGHT_GLSPOT = 5 };
enum { D3D7_MCS_MATERIAL = 0, D3D7_MCS_COLOR1 = 1, D3D7_MCS_COLOR2 = 2 };
enum { D3D7_COLOR_NONE = 0, D3D7_COLOR_RGB = 1, D3D7_COLOR_MONO = 2 };

#define D3D7_DP_DONOTCLIP        0x00000002u   /* D3DDP_* draw flags */
#define D3D7_DP_DONOTUPDATEEXTENTS 0x00000004u
#define D3D7_DP_DONOTLIGHT       0x00000008u
#define D3D7_DP_DONOTBATCH        0x00000020u

/* -------------------------------------------------- D3D7 structures ----- */
typedef struct { float x, y, z; } D3D7Vector;
typedef struct { float r, g, b, a; } D3D7ColorValue;
typedef struct { float m[16]; } D3D7Matrix;      /* row-major, like D3DMATRIX */
typedef struct { uint32_t x, y, w, h; float minz, maxz; } D3D7Viewport;

typedef struct {                                   /* D3DMATERIAL7: 17 floats */
    D3D7ColorValue diffuse, ambient, specular, emissive;
    float power;
} D3D7Material;

typedef struct {                                   /* D3DLIGHT7: 26 dwords */
    uint32_t type;                                 /* D3DLIGHTTYPE */
    D3D7ColorValue diffuse, specular, ambient;
    D3D7Vector position, direction;
    float range, falloff, att0, att1, att2, theta, phi;
} D3D7Light;

typedef struct {                                   /* D3DTLVERTEX: 32 bytes */
    float sx, sy, sz, rhw;
    uint32_t color, specular;
    float tu, tv;
} D3D7TLVertex;

typedef struct {                                   /* D3DLVERTEX: 32 bytes */
    float x, y, z;
    uint32_t reserved;
    uint32_t color, specular;
    float tu, tv;
} D3D7LVertex;

/* D3DPRIMCAPS: 14 dwords */
typedef struct {
    uint32_t dwSize, dwMiscCaps, dwRasterCaps, dwZCmpCaps, dwSrcBlendCaps,
             dwDestBlendCaps, dwAlphaCmpCaps, dwShadeCaps, dwTextureCaps,
             dwTextureFilterCaps, dwTextureBlendCaps, dwTextureAddressCaps,
             dwStippleWidth, dwStippleHeight;
} D3D7PrimCaps;

/* D3DDEVICEDESC7 (d3dcaps.h) - note: no dwSize/dwFlags members.
 * sizeof == 4 + 56 + 56 + 4 + 4 + 16 + 12 + 16 + 4 + 4 + 4 + 4 + 4 + 4 +
 *           4 + 4 + 16 + 4 + 4 + 16 = 232. dx7z allocates D3DDEVICEDESC7SIZE
 * (232) for it and passes it to EnumDevices' callback. */
typedef struct {
    uint32_t dwDevCaps;
    D3D7PrimCaps dpcLineCaps;
    D3D7PrimCaps dpcTriCaps;
    uint32_t dwDeviceRenderBitDepth;
    uint32_t dwDeviceZBufferBitDepth;
    uint32_t dwMinTextureWidth, dwMinTextureHeight;
    uint32_t dwMaxTextureWidth, dwMaxTextureHeight;
    uint32_t dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
    float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom;
    float dvExtentsAdjust;
    uint32_t dwStencilCaps;
    uint32_t dwFVFCaps;
    uint32_t dwTextureOpCaps;
    uint16_t wMaxTextureBlendStages;
    uint16_t wMaxSimultaneousTextures;
    uint32_t dwMaxActiveLights;
    float dvMaxVertexW;
    uint8_t deviceGUID[16];
    uint16_t wMaxUserClipPlanes;
    uint16_t wMaxVertexBlendMatrices;
    uint32_t dwVertexProcessingCaps;
    uint32_t dwReserved1, dwReserved2, dwReserved3, dwReserved4;
} D3D7DeviceDesc;

/* D3DDEVICEDESC (IDirect3D/IDirect3D2/IDirect3D3 EnumDevices callback, pre-7):
 * 252 bytes. Only the callback path of the IDirect3D3 face uses it. */
typedef struct { uint32_t dwSize, dwCaps; } D3D7TransformCaps;
typedef struct { uint32_t dwSize, dwCaps, dwLightingModel, dwNumLights; } D3D7LightingCaps;
typedef struct {
    uint32_t dwSize, dwFlags;
    uint32_t dcmColorModel;
    uint32_t dwDevCaps;
    D3D7TransformCaps dtcTransformCaps;
    uint32_t bClipping;
    D3D7LightingCaps dlcLightingCaps;
    D3D7PrimCaps dpcLineCaps, dpcTriCaps;
    uint32_t dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth;
    uint32_t dwMaxBufferSize, dwMaxVertexCount;
    uint32_t dwMinTextureWidth, dwMinTextureHeight, dwMaxTextureWidth, dwMaxTextureHeight;
    uint32_t dwMinStippleWidth, dwMaxStippleWidth, dwMinStippleHeight, dwMaxStippleHeight;
    uint32_t dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
    float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom;
    float dvExtentsAdjust;
    uint32_t dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
    uint16_t wMaxTextureBlendStages, wMaxSimultaneousTextures;
} D3D7DeviceDescV1;

/* DDPIXELFORMAT: 32 bytes */
typedef struct {
    uint32_t dwSize, dwFlags, dwFourCC, dwRGBBitCount;
    uint32_t dwRBitMask, dwGBitMask, dwBBitMask, dwRGBAlphaBitMask;
} D3D7PixelFormat;

/* DDSURFACEDESC2: 124 bytes (dx7z sets dwSize=0x7C and IDirectDraw7 expects
 * this structure, not the 108-byte DDSURFACEDESC). */
typedef struct {
    uint32_t dwSize, dwFlags, dwHeight, dwWidth, lPitch;
    uint32_t dwBackBufferCount;        /* / dwDepth */
    uint32_t dwMipMapCount;            /* / dwRefreshRate / dwSrcVBHandle */
    uint32_t dwAlphaBitDepth, dwReserved;
    uint32_t lpSurface;
    uint32_t ddckCKDestOverlayLo, ddckCKDestOverlayHi;
    uint32_t ddckCKDestBltLo, ddckCKDestBltHi;
    uint32_t ddckCKSrcOverlayLo, ddckCKSrcOverlayHi;
    uint32_t ddckCKSrcBltLo, ddckCKSrcBltHi;
    D3D7PixelFormat ddpfPixelFormat;
    uint32_t dwCaps;                   /* DDSCAPS2.dwCaps */
    uint32_t dwCaps2, dwCaps3, dwCaps4;
    uint32_t dwTextureStage;
} D3D7SurfaceDesc2;
#define D3D7_SURFACEDESC2_SIZE 124u

typedef struct { uint32_t lo, hi; } D3D7ColorKey;

typedef struct {                                    /* D3DRECT */
    int32_t x1, y1, x2, y2;
} D3D7Rect;

typedef struct {                                    /* D3DCLIPSTATUS */
    uint32_t dwFlags, dwStatus;
    float minx, maxx, miny, maxy, minz, maxz;
} D3D7ClipStatus;

typedef struct {                                    /* D3DVERTEXBUFFERDESC */
    uint32_t dwSize, dwCaps, dwFVF, dwNumVertices;
} D3D7VertexBufferDesc;

typedef struct {                                    /* D3DDRAWPRIMITIVESTRIDEDDATA */
    void *position;
    void *normal;
    void *diffuse;
    void *specular;
    void *textureCoords[8];
} D3D7DrawPrimitiveStridedData;

/* D3DFVF_* sizes: total vertex stride from an FVF code as D3D7 defines it.
 * Returns 0 for formats this port does not accept (blend weights, TEXCOORD >
 * 2, position types other than XYZ/XYZRHW). */
/* Number of floats per texture coordinate set for FVF texcoord index i.
 * D3D7 FVF bits 16+2i: 0 -> 2 (u,v), 1 -> 3 (u,v,w), 2 -> 4, 3 -> 1. */
static inline uint32_t d3d7_fvf_coord_floats(uint32_t fvf, uint32_t i) {
    uint32_t code = (fvf >> (16u + 2u * i)) & 3u;
    switch (code) {
    case 0: return 2;  /* 2D */
    case 1: return 3;  /* 3D */
    case 2: return 4;  /* 4D */
    default: return 1; /* 1D */
    }
}

static inline uint32_t d3d7_fvf_stride(uint32_t fvf) {
    uint32_t pos = fvf & D3D7_FVF_POSITION_MASK;
    if (pos != D3D7_FVF_XYZ && pos != D3D7_FVF_XYZRHW) return 0;
    uint32_t ntex = (fvf >> D3D7_FVF_TEXCOUNT_SHIFT) & 0xF;
    if (ntex > 2) return 0;
    uint32_t stride = pos == D3D7_FVF_XYZRHW ? 16u : 12u;
    if (fvf & D3D7_FVF_RESERVED1) stride += 4;      /* D3DLVERTEX dwReserved */
    if (fvf & D3D7_FVF_NORMAL)    stride += 12;
    if (fvf & D3D7_FVF_DIFFUSE)   stride += 4;
    if (fvf & D3D7_FVF_SPECULAR)  stride += 4;
    for (uint32_t i = 0; i < ntex; i++)
        stride += d3d7_fvf_coord_floats(fvf, i) * 4u;
    return stride;
}

/* D3DDD_* flags: what a D3DDEVICEDESC reports as valid */
#define D3D7_DDD_COLORMODEL          0x00000001u
#define D3D7_DDD_DEVCAPS             0x00000002u
#define D3D7_DDD_TRANSFORMCAPS       0x00000004u
#define D3D7_DDD_LIGHTINGCAPS        0x00000008u
#define D3D7_DDD_BCLIPPING           0x00000010u
#define D3D7_DDD_LINECAPS            0x00000020u
#define D3D7_DDD_TRICAPS             0x00000040u
#define D3D7_DDD_DEVICERENDERBITDEPTH 0x00000080u
#define D3D7_DDD_DEVICEZBUFFERBITDEPTH 0x00000100u
#define D3D7_DDD_MAXBUFFERSIZE       0x00000200u
#define D3D7_DDD_MAXVERTEXCOUNT      0x00000400u
#define D3D7_TRANSFORMCAPS_CLIP      0x00000001u
#define D3D7_LIGHTCAPS_POINT         0x00000001u
#define D3D7_LIGHTCAPS_SPOT          0x00000002u
#define D3D7_LIGHTCAPS_DIRECTIONAL   0x00000004u
#define D3D7_LIGHTINGMODEL_RGB       0x00000001u
#define D3D7_COLOR_MODEL_RGB         1u

/* -------------------------------------------------- capability bits ----- */
#define D3D7_DEVCAPS_FLOATTLVERTEX        0x00000001u
#define D3D7_DEVCAPS_EXECUTESYSTEMMEMORY  0x00000010u
#define D3D7_DEVCAPS_TLVERTEXSYSTEMMEMORY 0x00000040u
#define D3D7_DEVCAPS_TLVERTEXVIDEOMEMORY  0x00000080u
#define D3D7_DEVCAPS_TEXTURESYSTEMMEMORY  0x00000100u
#define D3D7_DEVCAPS_TEXTUREVIDEOMEMORY   0x00000200u
#define D3D7_DEVCAPS_DRAWPRIMTLVERTEX     0x00000400u
#define D3D7_DEVCAPS_CANRENDERAFTERFLIP   0x00000800u
#define D3D7_DEVCAPS_DRAWPRIMITIVES2      0x00002000u
#define D3D7_DEVCAPS_DRAWPRIMITIVES2EX    0x00008000u
#define D3D7_DEVCAPS_HWRASTERIZATION      0x00080000u
/* deliberately NOT D3DDEVCAPS_HWTRANSFORMANDLIGHT: THRASH pre-transforms */

#define D3D7_PMISCCAPS_CULLNONE  0x00000010u
#define D3D7_PMISCCAPS_CULLCW    0x00000020u
#define D3D7_PMISCCAPS_CULLCCW   0x00000040u

#define D3D7_PRASTERCAPS_DITHER       0x00000001u
#define D3D7_PRASTERCAPS_ZTEST       0x00000010u
#define D3D7_PRASTERCAPS_FOGVERTEX   0x00000080u
#define D3D7_PRASTERCAPS_FOGTABLE    0x00000100u
#define D3D7_PRASTERCAPS_MIPMAPLODBIAS 0x00002000u
#define D3D7_PRASTERCAPS_ZBIAS       0x00004000u
#define D3D7_PRASTERCAPS_ANISOTROPY  0x00020000u
#define D3D7_PRASTERCAPS_WFOG        0x00100000u
#define D3D7_PRASTERCAPS_ZFOG        0x00200000u

#define D3D7_PSHADECAPS_COLORGOURAUDRGB   0x00000008u
#define D3D7_PSHADECAPS_SPECULARGOURAUDRGB 0x00000200u
#define D3D7_PSHADECAPS_ALPHAGOURAUDBLEND  0x00004000u
#define D3D7_PSHADECAPS_FOGGOURAUD         0x00080000u

#define D3D7_PTEXTURECAPS_PERSPECTIVE  0x00000001u
#define D3D7_PTEXTURECAPS_POW2         0x00000002u
#define D3D7_PTEXTURECAPS_ALPHA        0x00000004u
#define D3D7_PTEXTURECAPS_TRANSPARENCY 0x00000008u
#define D3D7_PTEXTURECAPS_SQUAREONLY   0x00000020u

/* d3dcaps.h D3DPTFILTERCAPS_* (D3D7 MINF/MAGF/MIPF names) */
#define D3D7_PTFILTERCAPS_MINFPOINT   0x00000100u
#define D3D7_PTFILTERCAPS_MINFLINEAR  0x00000200u
#define D3D7_PTFILTERCAPS_MIPFPOINT   0x00010000u
#define D3D7_PTFILTERCAPS_MIPFLINEAR  0x00020000u
#define D3D7_PTFILTERCAPS_MAGFPOINT   0x01000000u
#define D3D7_PTFILTERCAPS_MAGFLINEAR  0x02000000u

#define D3D7_PBLENDCAPS_ZERO         0x00000001u
#define D3D7_PBLENDCAPS_ONE          0x00000002u
#define D3D7_PBLENDCAPS_SRCCOLOR     0x00000004u
#define D3D7_PBLENDCAPS_INVSRCCOLOR  0x00000008u
#define D3D7_PBLENDCAPS_SRCALPHA     0x00000010u
#define D3D7_PBLENDCAPS_INVSRCALPHA  0x00000020u
#define D3D7_PBLENDCAPS_DESTALPHA    0x00000040u
#define D3D7_PBLENDCAPS_INVDESTALPHA 0x00000080u
#define D3D7_PBLENDCAPS_DESTCOLOR    0x00000100u
#define D3D7_PBLENDCAPS_INVDESTCOLOR 0x00000200u
#define D3D7_PBLENDCAPS_SRCALPHASAT  0x00000400u

#define D3D7_PCMPCAPS_NEVER       0x00000001u
#define D3D7_PCMPCAPS_LESS        0x00000002u
#define D3D7_PCMPCAPS_EQUAL       0x00000004u
#define D3D7_PCMPCAPS_LESSEQUAL   0x00000008u
#define D3D7_PCMPCAPS_GREATER     0x00000010u
#define D3D7_PCMPCAPS_NOTEQUAL    0x00000020u
#define D3D7_PCMPCAPS_GREATEREQUAL 0x00000040u
#define D3D7_PCMPCAPS_ALWAYS      0x00000080u

#define D3D7_PTADDRESSCAPS_WRAP     0x00000001u
#define D3D7_PTADDRESSCAPS_MIRROR   0x00000002u
#define D3D7_PTADDRESSCAPS_CLAMP    0x00000004u
#define D3D7_PTADDRESSCAPS_BORDER   0x00000008u
#define D3D7_PTADDRESSCAPS_INDEPENDENTUV 0x00000010u

#define D3D7_DDBD_16 0x00000400u
#define D3D7_DDBD_24 0x00000200u
#define D3D7_DDBD_32 0x00000100u

#define D3D7_FVFCAPS_TEXCOORDCOUNTMASK 0x0000FFFFu
#define D3D7_FVFCAPS_DONOTSTRIPELEMENTS 0x00080000u
#define D3D7_VTXPCAPS_VERTEXFOG    0x00000004u
#define D3D7_VTXPCAPS_DIRECTIONALLIGHTS 0x00000008u
#define D3D7_VTXPCAPS_POSITIONALLIGHTS 0x00000010u
#define D3D7_VTXPCAPS_LOCALVIEWER  0x00000020u

#define D3D7_STENCILCAPS_KEEP    0x00000001u
#define D3D7_STENCILCAPS_ZERO    0x00000002u
#define D3D7_STENCILCAPS_REPLACE 0x00000004u

/* ------------------------------------------------- DirectDraw bits ------ */
#define DDSD_CAPS        0x00000001u
#define DDSD_HEIGHT      0x00000002u
#define DDSD_WIDTH       0x00000004u
#define DDSD_PITCH       0x00000008u
#define DDSD_PIXELFORMAT 0x00001000u
#define DDSD_MIPMAPCOUNT 0x00020000u
#define DDSD_BACKBUFFERCOUNT 0x00000020u
#define DDSD_ZBUFFERBITDEPTH 0x00000040u
#define DDSD_LPSURFACE   0x00000800u
#define DDSD_CKSRCBLT    0x00010000u

#define DDSCAPS_BACKBUFFER      0x00000004u
#define DDSCAPS_COMPLEX         0x00000008u
#define DDSCAPS_FLIP            0x00000010u
#define DDSCAPS_FRONTBUFFER     0x00000020u
#define DDSCAPS_OFFSCREENPLAIN  0x00000040u
#define DDSCAPS_PRIMARYSURFACE  0x00000200u
#define DDSCAPS_SYSTEMMEMORY    0x00000800u
#define DDSCAPS_TEXTURE         0x00001000u
#define DDSCAPS_3DDEVICE        0x00002000u
#define DDSCAPS_VIDEOMEMORY     0x00004000u
#define DDSCAPS_VISIBLE         0x00008000u
#define DDSCAPS_WRITEONLY       0x00010000u
#define DDSCAPS_ZBUFFER         0x00020000u
#define DDSCAPS_MIPMAP          0x00400000u
#define DDSCAPS2_MIPMAPSUBLEVEL 0x00000001u

#define DDPF_ALPHAPIXELS        0x00000001u
#define DDPF_ALPHA              0x00000002u
#define DDPF_PALETTEINDEXED8    0x00000020u
#define DDPF_RGB                0x00000040u
#define DDPF_ZBUFFER            0x00000400u

#define DDCKEY_SRCBLT           0x00000008u
#define DDCKEY_DESTBLT          0x00000001u

#endif /* NFS5_D3D7_ABI_H */
