/*
 * Wrapped IDirect3DDevice9 - FFP conversion layer.
 *
 * This is the core of the shader-to-fixed-function conversion proxy.
 * It intercepts shader-related D3D9 calls and replaces them with
 * fixed-function pipeline equivalents for RTX Remix compatibility.
 *
 * IDirect3DDevice9 has 119 methods (including IUnknown).
 * We intercept ~15 methods; the rest relay to the real device via
 * naked ASM thunks (zero overhead, no extra stack frames).
 *
 * Intercepted methods:
 *   - SetVertexShader / SetPixelShader: cache shader pointers
 *   - SetVertexShaderConstantF / SetPixelShaderConstantF: capture constants
 *   - DrawIndexedPrimitive: FFP conversion (NULL shaders, apply transforms)
 *   - DrawPrimitive: pass through with shaders (particles/UI)
 *   - SetVertexDeclaration: parse vertex elements, detect skinning
 *   - SetTexture / SetStreamSource: track state
 *   - BeginScene / EndScene / Present / Reset: frame lifecycle
 *
 * =====================================================================
 * HOW TO ADAPT THIS FOR YOUR GAME:
 *
 * 1. Run the analysis scripts against your game binary to discover:
 *    - VS constant register layout (which regs = View/Proj/World/Bones)
 *    - Vertex declaration formats
 *    - Which texture stage holds the albedo
 *
 * 2. Update the GAME-SPECIFIC #defines below with discovered values
 *
 * 3. Adjust draw call routing in WD_DrawIndexedPrimitive if needed
 *    (some games need different FFP bypass logic for skinned meshes,
 *    particles, UI, or multi-pass rendering)
 *
 * 4. Build with build.bat, deploy d3d9.dll + proxy.ini to game dir
 * =====================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* No-CRT memcpy: the compiler emits memcpy calls for struct/array copies */
#pragma function(memcpy)
void * __cdecl memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* Logging (from d3d9_main.c) */
extern void log_str(const char *s);
extern void log_hex(const char *prefix, unsigned int val);
extern void log_int(const char *prefix, int val);
extern void log_floats(const char *prefix, float *data, unsigned int count);
extern void log_float_val(const char *prefix, float f);
extern void log_floats_dec(const char *prefix, float *data, unsigned int count);

/* Forward declarations for game patches */
static int TRL_PatchFloat(unsigned int addr, float value, const char *name);

/* ============================================================
 * GAME-SPECIFIC: VS Constant Register Layout — Tomb Raider Legend
 *
 * Discovered from embedded D3DX shader constant tables (CTAB):
 *
 *   WorldViewProject  c0   4 regs  Combined WVP (non-skinned shaders)
 *   World             c4   3 regs  World matrix, 4x3 packed
 *   View              c8   2 regs  Partial view (lighting only)
 *   ViewProject       c12  4 regs  ViewProjection (skinned shaders)
 *   CameraPos         c16  1 reg   Camera world position
 *   ModulateColor0    c24  1 reg   Color tint
 *   TextureScroll     c26  1 reg   UV animation
 *   Constants         c39  1 reg   Utility {2.0, 0.5, 0.0, 1.0}
 *   SkinMatrices      c48  48 regs Bone palette (16 bones × 3)
 *
 * FFP strategy: World from c4 (expanded 4x3→4x4), ViewProjection
 * from c12, View set to identity. Falls back to WVP at c0 when
 * c4/c12 aren't populated.
 * ============================================================ */
#define VS_REG_WVP_START        0
#define VS_REG_WVP_END          4
#define VS_REG_WORLD_START      4
#define VS_REG_WORLD_END        8   /* 4 regs: full 4x4 (uploaded as part of c0 count=8) */
#define VS_REG_VIEW_START       8
#define VS_REG_VIEW_END        12   /* 4 regs: View matrix (uploaded as part of c8 count=8) */
#define VS_REG_VIEWPROJ_START  12
#define VS_REG_VIEWPROJ_END    16   /* 4 regs: ViewProjection (second half of c8 count=8) */

#define IS_WVP_REG(r)      ((r) >= VS_REG_WVP_START && (r) < VS_REG_WVP_END)
#define IS_WORLD_REG(r)    ((r) >= VS_REG_WORLD_START && (r) < VS_REG_WORLD_END)
#define IS_VIEW_REG(r)     ((r) >= VS_REG_VIEW_START && (r) < VS_REG_VIEW_END)
#define IS_VIEWPROJ_REG(r) ((r) >= VS_REG_VIEWPROJ_START && (r) < VS_REG_VIEWPROJ_END)

/* Game memory addresses — Tomb Raider Legend (discovered via RE + live tracing)
 * Fixed .data section addresses, no pointer indirection needed.
 */
#define TRL_VIEW_ADDR        0x010FC780  /* float[16] row-major View matrix   */
#define TRL_PROJ_ADDR        0x01002530  /* float[16] row-major Projection    */
#define TRL_FRUSTUM_THRESH   0x00EFDD64
#define TRL_CULL_PATCH_VA    0x0040EEA7
#define TRL_CULL_PATCH_LEN   15

/* Additional culling globals (discovered via static analysis) */
#define TRL_VIEW_DIST_ADDR   0x010FC910  /* float: max view distance for spatial tree */
#define TRL_FRUST_AABB_MIN   0x00EFD404  /* float: left/bottom frustum AABB bound (-1.0) */
#define TRL_FRUST_AABB_MAX   0x00EFD40C  /* float: right/top frustum AABB bound (1.0) */
#define TRL_FAR_CLIP_ADDR    0x00EFFECC  /* float: far clipping plane (12288.0) */
#define TRL_PARTICLE_NEAR    0x00EFE5EC  /* float: particle Z-depth near cull (20.0) */
#define TRL_PARTICLE_FADE    0x00EFFF0C  /* float: particle alpha fade distance (14336.0) */
#define TRL_PARTICLE_FADE2   0x00EFFF08  /* float: intermediate fade threshold (6144.0) */

/* Bone palette detection */
#define VS_REG_BONE_THRESHOLD  48
#define VS_REGS_PER_BONE        3   /* 4x3 packed per bone */
#define VS_BONE_MIN_REGS        9   /* Minimum 3 bones */

/* ============================================================
 * GAME-SPECIFIC: Skinning
 *
 * Set ENABLE_SKINNING to 1 to include FFP indexed vertex blending
 * code. This allows the proxy to convert skinned meshes to FFP.
 *
 * Caveats:
 *   - FFP indexed vertex blending supports max ~48 bones
 *   - Driver support varies (some GPUs limit to fewer bones)
 *   - Compressed vertex formats (SHORT4N normals) won't work in FFP
 *   - Most Remix workflows pass skinned meshes through with shaders
 *
 * The default behavior (even with ENABLE_SKINNING=1) is to pass
 * skinned meshes through with their original shaders. To actually
 * convert skinned draws to FFP, modify WD_DrawIndexedPrimitive.
 * ============================================================ */
#define ENABLE_SKINNING 1

/* ---- Diagnostic logging ---- */
#define DIAG_LOG_FRAMES 10
#define DIAG_DELAY_MS 15000
#define DIAG_ENABLED 1

#define DIAG_ACTIVE(self) \
    (DIAG_ENABLED && (self)->diagLoggedFrames < DIAG_LOG_FRAMES && \
     GetTickCount() - (self)->createTick >= DIAG_DELAY_MS)

/* ---- D3D9 Constants ---- */

#define D3DTS_VIEW          2
#define D3DTS_PROJECTION    3
#define D3DTS_WORLD         256
#define D3DTS_TEXTURE0      16

#define D3DRS_ZENABLE           7
#define D3DRS_FILLMODE          8
#define D3DFILL_SOLID           3
#define D3DRS_LIGHTING          137
#define D3DRS_AMBIENT           139
#define D3DRS_COLORVERTEX       141
#define D3DRS_SPECULARENABLE    29
#define D3DRS_DIFFUSEMATERIALSOURCE   145
#define D3DRS_AMBIENTMATERIALSOURCE   147
#define D3DRS_NORMALIZENORMALS  143
#define D3DRS_ALPHABLENDENABLE  27
#define D3DRS_SRCBLEND          19
#define D3DRS_DESTBLEND         20
#define D3DRS_CULLMODE          22
#define D3DRS_FOGENABLE         28

#define D3DTSS_COLOROP     1
#define D3DTSS_COLORARG1   2
#define D3DTSS_COLORARG2   3
#define D3DTSS_ALPHAOP     4
#define D3DTSS_ALPHAARG1   5
#define D3DTSS_ALPHAARG2   6
#define D3DTSS_TEXCOORDINDEX 11
#define D3DTSS_TEXTURETRANSFORMFLAGS 24

#define D3DTOP_DISABLE     1
#define D3DTOP_MODULATE    4
#define D3DTOP_SELECTARG1  2

#define D3DTA_TEXTURE      2
#define D3DTA_DIFFUSE      0
#define D3DTA_CURRENT      1

#define D3DLIGHT_DIRECTIONAL 3

#define D3DVBF_DISABLE  0
#define D3DVBF_1WEIGHTS  1
#define D3DVBF_2WEIGHTS  2
#define D3DVBF_3WEIGHTS  3

#define D3DRS_VERTEXBLEND              151
#define D3DRS_INDEXEDVERTEXBLENDENABLE  167

#define D3DTS_WORLDMATRIX(n) (256 + (n))

#define D3DDECL_END_STREAM 0xFF
#define D3DDECLUSAGE_POSITION     0
#define D3DDECLUSAGE_BLENDWEIGHT  1
#define D3DDECLUSAGE_BLENDINDICES 2
#define D3DDECLUSAGE_NORMAL       3
#define D3DDECLUSAGE_TEXCOORD     5
#define D3DDECLUSAGE_COLOR        10

#define MAX_FFP_BONES 48

#define D3DDECLTYPE_FLOAT3    2

/* ---- Device vtable slot indices ---- */
enum {
    SLOT_QueryInterface = 0,
    SLOT_AddRef = 1,
    SLOT_Release = 2,
    SLOT_TestCooperativeLevel = 3,
    SLOT_GetAvailableTextureMem = 4,
    SLOT_EvictManagedResources = 5,
    SLOT_GetDirect3D = 6,
    SLOT_GetDeviceCaps = 7,
    SLOT_GetDisplayMode = 8,
    SLOT_GetCreationParameters = 9,
    SLOT_SetCursorProperties = 10,
    SLOT_SetCursorPosition = 11,
    SLOT_ShowCursor = 12,
    SLOT_CreateAdditionalSwapChain = 13,
    SLOT_GetSwapChain = 14,
    SLOT_GetNumberOfSwapChains = 15,
    SLOT_Reset = 16,
    SLOT_Present = 17,
    SLOT_GetBackBuffer = 18,
    SLOT_GetRasterStatus = 19,
    SLOT_SetDialogBoxMode = 20,
    SLOT_SetGammaRamp = 21,
    SLOT_GetGammaRamp = 22,
    SLOT_CreateTexture = 23,
    SLOT_CreateVolumeTexture = 24,
    SLOT_CreateCubeTexture = 25,
    SLOT_CreateVertexBuffer = 26,
    SLOT_CreateIndexBuffer = 27,
    SLOT_CreateRenderTarget = 28,
    SLOT_CreateDepthStencilSurface = 29,
    SLOT_UpdateSurface = 30,
    SLOT_UpdateTexture = 31,
    SLOT_GetRenderTargetData = 32,
    SLOT_GetFrontBufferData = 33,
    SLOT_StretchRect = 34,
    SLOT_ColorFill = 35,
    SLOT_CreateOffscreenPlainSurface = 36,
    SLOT_SetRenderTarget = 37,
    SLOT_GetRenderTarget = 38,
    SLOT_SetDepthStencilSurface = 39,
    SLOT_GetDepthStencilSurface = 40,
    SLOT_BeginScene = 41,
    SLOT_EndScene = 42,
    SLOT_Clear = 43,
    SLOT_SetTransform = 44,
    SLOT_GetTransform = 45,
    SLOT_MultiplyTransform = 46,
    SLOT_SetViewport = 47,
    SLOT_GetViewport = 48,
    SLOT_SetMaterial = 49,
    SLOT_GetMaterial = 50,
    SLOT_SetLight = 51,
    SLOT_GetLight = 52,
    SLOT_LightEnable = 53,
    SLOT_GetLightEnable = 54,
    SLOT_SetClipPlane = 55,
    SLOT_GetClipPlane = 56,
    SLOT_SetRenderState = 57,
    SLOT_GetRenderState = 58,
    SLOT_CreateStateBlock = 59,
    SLOT_BeginStateBlock = 60,
    SLOT_EndStateBlock = 61,
    SLOT_SetClipStatus = 62,
    SLOT_GetClipStatus = 63,
    SLOT_GetTexture = 64,
    SLOT_SetTexture = 65,
    SLOT_GetTextureStageState = 66,
    SLOT_SetTextureStageState = 67,
    SLOT_GetSamplerState = 68,
    SLOT_SetSamplerState = 69,
    SLOT_ValidateDevice = 70,
    SLOT_SetPaletteEntries = 71,
    SLOT_GetPaletteEntries = 72,
    SLOT_SetCurrentTexturePalette = 73,
    SLOT_GetCurrentTexturePalette = 74,
    SLOT_SetScissorRect = 75,
    SLOT_GetScissorRect = 76,
    SLOT_SetSoftwareVertexProcessing = 77,
    SLOT_GetSoftwareVertexProcessing = 78,
    SLOT_SetNPatchMode = 79,
    SLOT_GetNPatchMode = 80,
    SLOT_DrawPrimitive = 81,
    SLOT_DrawIndexedPrimitive = 82,
    SLOT_DrawPrimitiveUP = 83,
    SLOT_DrawIndexedPrimitiveUP = 84,
    SLOT_ProcessVertices = 85,
    SLOT_CreateVertexDeclaration = 86,
    SLOT_SetVertexDeclaration = 87,
    SLOT_GetVertexDeclaration = 88,
    SLOT_SetFVF = 89,
    SLOT_GetFVF = 90,
    SLOT_CreateVertexShader = 91,
    SLOT_SetVertexShader = 92,
    SLOT_GetVertexShader = 93,
    SLOT_SetVertexShaderConstantF = 94,
    SLOT_GetVertexShaderConstantF = 95,
    SLOT_SetVertexShaderConstantI = 96,
    SLOT_GetVertexShaderConstantI = 97,
    SLOT_SetVertexShaderConstantB = 98,
    SLOT_GetVertexShaderConstantB = 99,
    SLOT_SetStreamSource = 100,
    SLOT_GetStreamSource = 101,
    SLOT_SetStreamSourceFreq = 102,
    SLOT_GetStreamSourceFreq = 103,
    SLOT_SetIndices = 104,
    SLOT_GetIndices = 105,
    SLOT_CreatePixelShader = 106,
    SLOT_SetPixelShader = 107,
    SLOT_GetPixelShader = 108,
    SLOT_SetPixelShaderConstantF = 109,
    SLOT_GetPixelShaderConstantF = 110,
    SLOT_SetPixelShaderConstantI = 111,
    SLOT_GetPixelShaderConstantI = 112,
    SLOT_SetPixelShaderConstantB = 113,
    SLOT_GetPixelShaderConstantB = 114,
    SLOT_DrawRectPatch = 115,
    SLOT_DrawTriPatch = 116,
    SLOT_DeletePatch = 117,
    SLOT_CreateQuery = 118,
    DEVICE_VTABLE_SIZE = 119
};

/* ---- WrappedDevice ---- */

typedef struct WrappedDevice {
    void **vtbl;
    void *pReal;            /* real IDirect3DDevice9* */
    int refCount;
    unsigned int frameCount;
    int ffpSetup;           /* whether FFP state has been configured this frame */

    float vsConst[256 * 4]; /* vertex shader constants (up to 256 vec4) */
    float psConst[32 * 4];  /* pixel shader constants (up to 32 vec4) */
    int worldDirty;         /* c4-c6 World changed since last SetTransform */
    int viewProjDirty;      /* c12-c15 ViewProjection changed since last SetTransform */
    int wvpDirty;           /* c0-c3 WorldViewProject changed */
    int psConstDirty;

    /* VP inverse cache: reuse the previous inverse when VP hasn't changed
     * (within epsilon), eliminating floating-point jitter in the World
     * matrix that breaks Remix hash stability for static objects.
     * Recomputed from game memory every draw to handle multi-pass rendering
     * (shadow maps, reflections) where VP changes mid-scene. */
    float prevVP[16];
    float prevVpInv[16];
    int prevVpInvValid;

    void *lastVS;           /* last vertex shader set by the game */
    void *lastPS;           /* last pixel shader set by the game */
    int viewProjValid;      /* set once ViewProjection (c12) has been written */
    int wvpValid;           /* set once WorldViewProject (c0) has been written */
    int ffpActive;          /* real device currently has NULL shaders (FFP mode) */

#if ENABLE_SKINNING
    /* Skinning detection */
    void *lastDecl;
    int curDeclIsSkinned;
    int curDeclNumWeights;
    int boneStartReg;
    int numBones;
    int skinningSetup;
    int lastVsConstWriteEnd;
#else
    void *lastDecl;
    int curDeclIsSkinned;
#endif

    /* Vertex element tracking */
    int curDeclHasTexcoord;
    int curDeclHasNormal;
    int curDeclHasColor;
    int curDeclPosIsFloat3;  /* POSITION element is FLOAT3 (screen-space vertex format) */
    int curDeclNumElems;
    unsigned char curDeclElems[8 * 20];

    /* Texture tracking (stages 0-7) */
    void *curTexture[8];
    int albedoStage;
    int disableNormalMaps;

    /* Stream source tracking (streams 0-3) */
    void *streamVB[4];
    unsigned int streamOffset[4];
    unsigned int streamStride[4];

    /* Diagnostic state */
    void *loggedDecls[32];
    int loggedDeclCount;
    void *diagTexSeen[8][32];
    int diagTexUniq[8];
    unsigned int createTick;
    unsigned int diagLoggedFrames;
    unsigned int drawCallCount;
    unsigned int sceneCount;
    unsigned int diagVsLogged;
    unsigned int diagDipLogged;
    int vsConstWriteLog[256];

    /* Per-frame routing counters (reset each Present) */
    unsigned int ffpDraws;
    unsigned int skinnedDraws;
    unsigned int quadSkips;
    unsigned int float3Passes;
    unsigned int shaderDraws;
    unsigned int decomposedCount;
    unsigned int fallbackCount;
} WrappedDevice;

#define REAL(self) (((WrappedDevice*)(self))->pReal)
#define REAL_VT(self) (*(void***)(REAL(self)))

static __inline void** RealVtbl(WrappedDevice *self) {
    return *(void***)(self->pReal);
}

static __inline void shader_addref(void *pShader) {
    if (pShader) {
        typedef unsigned long (__stdcall *FN)(void*);
        ((FN)(*(void***)pShader)[1])(pShader);
    }
}
static __inline void shader_release(void *pShader) {
    if (pShader) {
        typedef unsigned long (__stdcall *FN)(void*);
        ((FN)(*(void***)pShader)[2])(pShader);
    }
}

/* Forward declarations */
static void TRL_EnforceViewDistance(void);

/* ---- FFP State Setup ---- */

typedef struct {
    float Diffuse[4];
    float Ambient[4];
    float Specular[4];
    float Emissive[4];
    float Power;
} D3DMATERIAL9;

/*
 * Setup lighting for FFP mode.
 * Disables FFP lighting since vertex declarations typically lack normals
 * and RTX Remix handles lighting via ray tracing. Sets a white material
 * so unlit FFP output is visible.
 */
static void FFP_SetupLighting(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRenderState)(void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetMaterial)(void*, D3DMATERIAL9*);
    void **vt = RealVtbl(self);
    D3DMATERIAL9 mat;
    int i;

    ((FN_SetRenderState)vt[SLOT_SetRenderState])(self->pReal, D3DRS_LIGHTING, 0);
    ((FN_SetRenderState)vt[SLOT_SetRenderState])(self->pReal, D3DRS_COLORVERTEX, 0);
    ((FN_SetRenderState)vt[SLOT_SetRenderState])(self->pReal, D3DRS_SPECULARENABLE, 0);
    ((FN_SetRenderState)vt[SLOT_SetRenderState])(self->pReal, D3DRS_AMBIENT, 0xFFFFFFFFu);
    ((FN_SetRenderState)vt[SLOT_SetRenderState])(self->pReal, D3DRS_CULLMODE, 1); /* D3DCULL_NONE */

    for (i = 0; i < 4; i++) {
        mat.Diffuse[i] = 1.0f;
        mat.Ambient[i] = 1.0f;
        mat.Specular[i] = 0.0f;
        mat.Emissive[i] = 0.0f;
    }
    mat.Power = 0.0f;
    ((FN_SetMaterial)vt[SLOT_SetMaterial])(self->pReal, &mat);
}

/*
 * Setup texture stages for FFP mode.
 * Stage 0: modulate texture color with vertex/material diffuse.
 * Stage 1+: disabled.
 */
static void FFP_SetupTextureStages(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTSS)(void*, unsigned int, unsigned int, unsigned int);
    void **vt = RealVtbl(self);

    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAARG2, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_TEXCOORDINDEX, 0);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);

    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

/* Transpose a 4x4 matrix (column-major -> row-major or vice versa) */
static void mat4_transpose(float *dst, const float *src) {
    dst[0]  = src[0];  dst[1]  = src[4];  dst[2]  = src[8];  dst[3]  = src[12];
    dst[4]  = src[1];  dst[5]  = src[5];  dst[6]  = src[9];  dst[7]  = src[13];
    dst[8]  = src[2];  dst[9]  = src[6];  dst[10] = src[10]; dst[11] = src[14];
    dst[12] = src[3];  dst[13] = src[7];  dst[14] = src[11]; dst[15] = src[15];
}

/* Log a 4x4 matrix row by row (for diagnostics) */
static void diag_log_matrix(const char *name, const float *m) {
    log_str(name);
    log_str(":\r\n");
    log_floats_dec("  row0: ", (float*)&m[0], 4);
    log_floats_dec("  row1: ", (float*)&m[4], 4);
    log_floats_dec("  row2: ", (float*)&m[8], 4);
    log_floats_dec("  row3: ", (float*)&m[12], 4);
}

static const float s_identity[16] = {
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
};

/* Multiply two row-major 4x4 matrices: dst = A * B */
static void mat4_mul(float *dst, const float *a, const float *b) {
    int i, j, k;
    float tmp[16];
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            tmp[i*4+j] = 0.0f;
            for (k = 0; k < 4; k++)
                tmp[i*4+j] += a[i*4+k] * b[k*4+j];
        }
    }
    for (i = 0; i < 16; i++) dst[i] = tmp[i];
}

/*
 * Invert an affine 4x4 matrix (rotation+translation).
 * For affine M = [R|t; 0 0 0 1], inverse = [R^T | -R^T*t; 0 0 0 1].
 */
static void mat4_affine_inverse(float *dst, const float *src) {
    /* Transpose the 3x3 rotation part */
    dst[0] = src[0]; dst[1] = src[4]; dst[2]  = src[8];  dst[3]  = 0.0f;
    dst[4] = src[1]; dst[5] = src[5]; dst[6]  = src[9];  dst[7]  = 0.0f;
    dst[8] = src[2]; dst[9] = src[6]; dst[10] = src[10]; dst[11] = 0.0f;
    /* -R^T * t */
    dst[12] = -(dst[0]*src[12] + dst[4]*src[13] + dst[8]*src[14]);
    dst[13] = -(dst[1]*src[12] + dst[5]*src[13] + dst[9]*src[14]);
    dst[14] = -(dst[2]*src[12] + dst[6]*src[13] + dst[10]*src[14]);
    dst[15] = 1.0f;
}

/* General 4x4 matrix inverse using cofactor expansion (row-major). Returns 1 on success. */
static int mat4_inverse(float *dst, const float *m) {
    float inv[16], det;
    int i;

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return 0;
    det = 1.0f / det;
    for (i = 0; i < 16; i++) dst[i] = inv[i] * det;
    return 1;
}

/*
 * Quantize a 4x4 matrix to eliminate floating-point jitter.
 *
 * The WVP decomposition (World = WVP * inv(VP)) produces slightly different
 * World matrices for the same static object when VP changes (camera moves).
 * Remix hashes geometry by vertex data + World transform, so sub-ULP jitter
 * causes hash instability. Snapping each element to a grid eliminates this.
 *
 * Grid size 1e-3 is larger than typical FP decomposition error (~1e-5)
 * but small enough to preserve spatial accuracy (0.001 world units ≈ <1mm).
 */
static void mat4_quantize(float *m, float grid) {
    float inv_grid = 1.0f / grid;
    int i;
    for (i = 0; i < 16; i++) {
        float v = m[i] * inv_grid;
        /* Round to nearest integer, then scale back */
        m[i] = (float)(int)(v + (v >= 0.0f ? 0.5f : -0.5f)) * grid;
    }
}

/*
 * Apply captured VS constants as FFP transforms.
 *
 * TRL uploads a combined WVP to c0-c3 for ALL shaders. There is no
 * separate World/View/Projection in constants. c8-c15 are NOT matrices.
 *
 * Strategy: Full decomposition — read View and Projection from game memory
 * every draw, compute World = WVP * inv(View * Proj). The VP inverse is
 * cached and reused when VP hasn't changed (within 1e-4 epsilon), which
 * eliminates floating-point jitter in the decomposed World matrix that
 * breaks Remix hash stability for static objects. VP is recomputed from
 * game memory every draw to handle multi-pass rendering (shadow maps,
 * reflections) where the game changes View/Proj mid-scene.
 *
 * fusedWorldViewMode=0 with proper W/V/P separation enables path tracing.
 */
static void FFP_ApplyTransforms(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
    void **vt = RealVtbl(self);
    float wvpRM[16], world[16], vp[16], vpInv[16];
    float *gameView = (float *)TRL_VIEW_ADDR;
    float *gameProj = (float *)TRL_PROJ_ADDR;
    int haveVpInv = 0;
    int decomposed = 0;

    if (!self->wvpDirty) return;
    if (!self->wvpValid) return;

    /* c0-c3 in VS constants are columns of WVP; transpose to row-major for D3D */
    mat4_transpose(wvpRM, &self->vsConst[VS_REG_WVP_START * 4]);

    /* Compute VP from current game memory every draw.
     * Reuse cached inverse when VP matches previous (within epsilon) —
     * eliminates FP jitter for hash stability without stale-caching
     * across render passes. */
    mat4_mul(vp, gameView, gameProj);

    if (self->prevVpInvValid) {
        int same = 1;
        int ci;
        for (ci = 0; ci < 16; ci++) {
            float diff = vp[ci] - self->prevVP[ci];
            if (diff > 1e-4f || diff < -1e-4f) { same = 0; break; }
        }
        if (same) {
            int cj;
            for (cj = 0; cj < 16; cj++) vpInv[cj] = self->prevVpInv[cj];
            haveVpInv = 1;
        }
    }

    if (!haveVpInv) {
        haveVpInv = mat4_inverse(vpInv, vp);
        if (haveVpInv) {
            int ck;
            for (ck = 0; ck < 16; ck++) {
                self->prevVP[ck] = vp[ck];
                self->prevVpInv[ck] = vpInv[ck];
            }
            self->prevVpInvValid = 1;
        }
    }

    if (haveVpInv) {
        mat4_mul(world, wvpRM, vpInv);
        mat4_quantize(world, 1e-3f);

        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD, world);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, gameView);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, gameProj);
        decomposed = 1;
        self->decomposedCount++;
    } else {
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD, wvpRM);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, (float*)s_identity);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, (float*)s_identity);
        self->fallbackCount++;
    }

#if DIAG_ENABLED
    if (DIAG_ACTIVE(self) && self->diagVsLogged < 5) {
        if (decomposed) {
            log_str("  FFP_Transforms: full decomp (W/V/P separate)\r\n");
            diag_log_matrix("    world", world);
            diag_log_matrix("    gameView", gameView);
            diag_log_matrix("    gameProj", gameProj);
        } else {
            log_str("  FFP_Transforms: fallback (WVP-as-World)\r\n");
            diag_log_matrix("    wvpRM", wvpRM);
        }
        self->diagVsLogged++;
    }
#endif

    self->wvpDirty = 0;
}

#if ENABLE_SKINNING
/*
 * Upload bone matrices for FFP indexed vertex blending.
 *
 * Each bone occupies VS_REGS_PER_BONE consecutive vec4 registers
 * (typically 3 = 4x3 matrix, rows 0-2; row 3 = implicit 0,0,0,1).
 * Uploaded as D3DTS_WORLDMATRIX(n) for FFP indexed vertex blending.
 */
static void FFP_UploadBones(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    void **vt = RealVtbl(self);
    int startReg, numBones, i;
    float boneMat[16];

    startReg = self->boneStartReg;
    if (startReg < (int)VS_REG_BONE_THRESHOLD) return;

    numBones = self->numBones;
    if (numBones <= 0) return;
    if (numBones > MAX_FFP_BONES) numBones = MAX_FFP_BONES;

    ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_INDEXEDVERTEXBLENDENABLE, 1);
    ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_VERTEXBLEND,
        self->curDeclNumWeights == 1 ? D3DVBF_1WEIGHTS :
        self->curDeclNumWeights == 2 ? D3DVBF_2WEIGHTS : D3DVBF_3WEIGHTS);

    for (i = 0; i < numBones; i++) {
        int base = (startReg + i * VS_REGS_PER_BONE) * 4;
        float *src = &self->vsConst[base];

        /* Build 4x4 from 4x3 packed (transpose column->row major) */
        boneMat[0]  = src[0];  boneMat[1]  = src[4];  boneMat[2]  = src[8];   boneMat[3]  = 0.0f;
        boneMat[4]  = src[1];  boneMat[5]  = src[5];  boneMat[6]  = src[9];   boneMat[7]  = 0.0f;
        boneMat[8]  = src[2];  boneMat[9]  = src[6];  boneMat[10] = src[10];  boneMat[11] = 0.0f;
        boneMat[12] = src[3];  boneMat[13] = src[7];  boneMat[14] = src[11];  boneMat[15] = 1.0f;

        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal,
            D3DTS_WORLDMATRIX(i), boneMat);
    }

    self->skinningSetup = 1;
}

static void FFP_DisableSkinning(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    void **vt = RealVtbl(self);

    if (self->skinningSetup) {
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_INDEXEDVERTEXBLENDENABLE, 0);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
        self->skinningSetup = 0;
    }
}
#endif /* ENABLE_SKINNING */

/*
 * Apply decomposed transforms for Remix without changing shaders.
 *
 * The game's vertex shaders handle SHORT4 position transforms correctly.
 * Remix's vertex capture intercepts the shader output and uses our
 * SetTransform W/V/P to compute world-space positions for ray tracing.
 *
 * We keep shaders active (Remix captures post-VS positions) and only
 * override dxwrapper's identity View/Proj with the real camera matrices.
 */
static void FFP_Engage(WrappedDevice *self) {
    /* Force re-application of transforms every draw because dxwrapper
       overwrites them with identity View/Proj between draws. */
    self->wvpDirty = 1;
    FFP_ApplyTransforms(self);
    self->ffpActive = 1;
}

/* No shader restoration needed — we never null them */
static void FFP_Disengage(WrappedDevice *self) {
    self->ffpActive = 0;
}

/* ---- Vtable method implementations ---- */

static void *s_device_vtbl[DEVICE_VTABLE_SIZE];

/* 0: QueryInterface */
static int __stdcall WD_QueryInterface(WrappedDevice *self, void *riid, void **ppv) {
    typedef int (__stdcall *FN)(void*, void*, void**);
    return ((FN)RealVtbl(self)[0])(self->pReal, riid, ppv);
}

/* 1: AddRef */
static unsigned long __stdcall WD_AddRef(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN)(void*);
    self->refCount++;
    return ((FN)RealVtbl(self)[1])(self->pReal);
}

/* 2: Release */
static unsigned long __stdcall WD_Release(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN)(void*);
    unsigned long rc = ((FN)RealVtbl(self)[2])(self->pReal);
    self->refCount--;
    if (self->refCount <= 0) {
        log_str("WrappedDevice released\r\n");
        shader_release(self->lastVS);
        shader_release(self->lastPS);
        self->lastVS = NULL;
        self->lastPS = NULL;
        HeapFree(GetProcessHeap(), 0, self);
    }
    return rc;
}

/* ---- Relay thunks for non-intercepted methods ---- */

#ifdef _MSC_VER
/* MSVC x86 naked thunks: replace 'this' with pReal and jump to real vtable */
#define RELAY_THUNK(name, slot) \
    static __declspec(naked) void __stdcall name(void) { \
        __asm { mov eax, [esp+4] }      /* eax = WrappedDevice* */ \
        __asm { mov ecx, [eax+4] }      /* ecx = pReal */ \
        __asm { mov [esp+4], ecx }      /* replace this */ \
        __asm { mov eax, [ecx] }        /* eax = real vtable */ \
        __asm { jmp dword ptr [eax + slot*4] } \
    }

RELAY_THUNK(Relay_03, 3)    /* TestCooperativeLevel */
RELAY_THUNK(Relay_04, 4)    /* GetAvailableTextureMem */
RELAY_THUNK(Relay_05, 5)    /* EvictManagedResources */
RELAY_THUNK(Relay_06, 6)    /* GetDirect3D */
RELAY_THUNK(Relay_07, 7)    /* GetDeviceCaps */
RELAY_THUNK(Relay_08, 8)    /* GetDisplayMode */
RELAY_THUNK(Relay_09, 9)    /* GetCreationParameters */
RELAY_THUNK(Relay_10, 10)   /* SetCursorProperties */
RELAY_THUNK(Relay_11, 11)   /* SetCursorPosition */
RELAY_THUNK(Relay_12, 12)   /* ShowCursor */
RELAY_THUNK(Relay_13, 13)   /* CreateAdditionalSwapChain */
RELAY_THUNK(Relay_14, 14)   /* GetSwapChain */
RELAY_THUNK(Relay_15, 15)   /* GetNumberOfSwapChains */
RELAY_THUNK(Relay_18, 18)   /* GetBackBuffer */
RELAY_THUNK(Relay_19, 19)   /* GetRasterStatus */
RELAY_THUNK(Relay_20, 20)   /* SetDialogBoxMode */
RELAY_THUNK(Relay_21, 21)   /* SetGammaRamp */
RELAY_THUNK(Relay_22, 22)   /* GetGammaRamp */
RELAY_THUNK(Relay_23, 23)   /* CreateTexture */
RELAY_THUNK(Relay_24, 24)   /* CreateVolumeTexture */
RELAY_THUNK(Relay_25, 25)   /* CreateCubeTexture */
RELAY_THUNK(Relay_26, 26)   /* CreateVertexBuffer */
RELAY_THUNK(Relay_27, 27)   /* CreateIndexBuffer */
RELAY_THUNK(Relay_28, 28)   /* CreateRenderTarget */
RELAY_THUNK(Relay_29, 29)   /* CreateDepthStencilSurface */
RELAY_THUNK(Relay_30, 30)   /* UpdateSurface */
RELAY_THUNK(Relay_31, 31)   /* UpdateTexture */
RELAY_THUNK(Relay_32, 32)   /* GetRenderTargetData */
RELAY_THUNK(Relay_33, 33)   /* GetFrontBufferData */
RELAY_THUNK(Relay_34, 34)   /* StretchRect */
RELAY_THUNK(Relay_35, 35)   /* ColorFill */
RELAY_THUNK(Relay_36, 36)   /* CreateOffscreenPlainSurface */
RELAY_THUNK(Relay_37, 37)   /* SetRenderTarget */
RELAY_THUNK(Relay_38, 38)   /* GetRenderTarget */
RELAY_THUNK(Relay_39, 39)   /* SetDepthStencilSurface */
RELAY_THUNK(Relay_40, 40)   /* GetDepthStencilSurface */
RELAY_THUNK(Relay_43, 43)   /* Clear */
RELAY_THUNK(Relay_44, 44)   /* SetTransform */
RELAY_THUNK(Relay_45, 45)   /* GetTransform */
RELAY_THUNK(Relay_46, 46)   /* MultiplyTransform */
RELAY_THUNK(Relay_47, 47)   /* SetViewport */
RELAY_THUNK(Relay_48, 48)   /* GetViewport */
RELAY_THUNK(Relay_49, 49)   /* SetMaterial */
RELAY_THUNK(Relay_50, 50)   /* GetMaterial */
RELAY_THUNK(Relay_51, 51)   /* SetLight */
RELAY_THUNK(Relay_52, 52)   /* GetLight */
RELAY_THUNK(Relay_53, 53)   /* LightEnable */
RELAY_THUNK(Relay_54, 54)   /* GetLightEnable */
RELAY_THUNK(Relay_55, 55)   /* SetClipPlane */
RELAY_THUNK(Relay_56, 56)   /* GetClipPlane */
RELAY_THUNK(Relay_57, 57)   /* SetRenderState */
RELAY_THUNK(Relay_58, 58)   /* GetRenderState */
RELAY_THUNK(Relay_59, 59)   /* CreateStateBlock */
RELAY_THUNK(Relay_60, 60)   /* BeginStateBlock */
RELAY_THUNK(Relay_61, 61)   /* EndStateBlock */
RELAY_THUNK(Relay_62, 62)   /* SetClipStatus */
RELAY_THUNK(Relay_63, 63)   /* GetClipStatus */
RELAY_THUNK(Relay_64, 64)   /* GetTexture */
RELAY_THUNK(Relay_66, 66)   /* GetTextureStageState */
RELAY_THUNK(Relay_67, 67)   /* SetTextureStageState */
RELAY_THUNK(Relay_68, 68)   /* GetSamplerState */
RELAY_THUNK(Relay_69, 69)   /* SetSamplerState */
RELAY_THUNK(Relay_70, 70)   /* ValidateDevice */
RELAY_THUNK(Relay_71, 71)   /* SetPaletteEntries */
RELAY_THUNK(Relay_72, 72)   /* GetPaletteEntries */
RELAY_THUNK(Relay_73, 73)   /* SetCurrentTexturePalette */
RELAY_THUNK(Relay_74, 74)   /* GetCurrentTexturePalette */
RELAY_THUNK(Relay_75, 75)   /* SetScissorRect */
RELAY_THUNK(Relay_76, 76)   /* GetScissorRect */
RELAY_THUNK(Relay_77, 77)   /* SetSoftwareVertexProcessing */
RELAY_THUNK(Relay_78, 78)   /* GetSoftwareVertexProcessing */
RELAY_THUNK(Relay_79, 79)   /* SetNPatchMode */
RELAY_THUNK(Relay_80, 80)   /* GetNPatchMode */
/* DrawPrimitiveUP and DrawIndexedPrimitiveUP intercepted to suppress line primitives */
RELAY_THUNK(Relay_85, 85)   /* ProcessVertices */
RELAY_THUNK(Relay_86, 86)   /* CreateVertexDeclaration */
RELAY_THUNK(Relay_88, 88)   /* GetVertexDeclaration */
RELAY_THUNK(Relay_89, 89)   /* SetFVF */
RELAY_THUNK(Relay_90, 90)   /* GetFVF */
RELAY_THUNK(Relay_91, 91)   /* CreateVertexShader */
RELAY_THUNK(Relay_93, 93)   /* GetVertexShader */
RELAY_THUNK(Relay_95, 95)   /* GetVertexShaderConstantF */
RELAY_THUNK(Relay_96, 96)   /* SetVertexShaderConstantI */
RELAY_THUNK(Relay_97, 97)   /* GetVertexShaderConstantI */
RELAY_THUNK(Relay_98, 98)   /* SetVertexShaderConstantB */
RELAY_THUNK(Relay_99, 99)   /* GetVertexShaderConstantB */
RELAY_THUNK(Relay_101, 101) /* GetStreamSource */
RELAY_THUNK(Relay_102, 102) /* SetStreamSourceFreq */
RELAY_THUNK(Relay_103, 103) /* GetStreamSourceFreq */
RELAY_THUNK(Relay_104, 104) /* SetIndices */
RELAY_THUNK(Relay_105, 105) /* GetIndices */
RELAY_THUNK(Relay_106, 106) /* CreatePixelShader */
RELAY_THUNK(Relay_108, 108) /* GetPixelShader */
RELAY_THUNK(Relay_110, 110) /* GetPixelShaderConstantF */
RELAY_THUNK(Relay_111, 111) /* SetPixelShaderConstantI */
RELAY_THUNK(Relay_112, 112) /* GetPixelShaderConstantI */
RELAY_THUNK(Relay_113, 113) /* SetPixelShaderConstantB */
RELAY_THUNK(Relay_114, 114) /* GetPixelShaderConstantB */
RELAY_THUNK(Relay_115, 115) /* DrawRectPatch */
RELAY_THUNK(Relay_116, 116) /* DrawTriPatch */
RELAY_THUNK(Relay_117, 117) /* DeletePatch */
RELAY_THUNK(Relay_118, 118) /* CreateQuery */

#else
#error "Only MSVC x86 is supported (needs __declspec(naked) + inline asm)"
#endif

/* ---- Intercepted method implementations ---- */

/* 16: Reset — invalidates all resources */
static int __stdcall WD_Reset(WrappedDevice *self, void *pPresentParams) {
    typedef int (__stdcall *FN)(void*, void*);
    int hr;

    log_str("== Device Reset ==\r\n");

    shader_release(self->lastVS);
    shader_release(self->lastPS);
    self->lastVS = NULL;
    self->lastPS = NULL;
    self->viewProjValid = 0;
    self->wvpValid = 0;
    self->ffpSetup = 0;
    self->worldDirty = 0;
    self->viewProjDirty = 0;
    self->wvpDirty = 0;
    self->psConstDirty = 0;
    self->ffpActive = 0;
    self->prevVpInvValid = 0;

    hr = ((FN)RealVtbl(self)[SLOT_Reset])(self->pReal, pPresentParams);
    log_hex("  Reset hr=", hr);
    return hr;
}

/* 17: Present */
static int __stdcall WD_Present(WrappedDevice *self, void *a, void *b, void *c, void *d) {
    typedef int (__stdcall *FN)(void*, void*, void*, void*, void*);
    int hr;

#if DIAG_ENABLED
    if (DIAG_ACTIVE(self)) {
        log_str("==== PRESENT frame ");
        log_int("", self->frameCount);
        log_int("  diagFrame: ", self->diagLoggedFrames);
        log_int("  drawCalls: ", self->drawCallCount);
        log_int("  scenes: ", self->sceneCount);
        {
            int r;
            log_str("  VS regs written: ");
            for (r = 0; r < 256; r++) {
                if (self->vsConstWriteLog[r]) {
                    log_int("c", r);
                }
            }
            log_str("\r\n");
        }
        {
            int ts;
            log_str("  Unique textures per stage:\r\n");
            for (ts = 0; ts < 8; ts++) {
                if (self->diagTexUniq[ts] > 0) {
                    log_int("    stage ", ts);
                    log_int("      unique=", self->diagTexUniq[ts]);
                }
            }
        }
        log_str("  Draw routing:\r\n");
        log_int("    FFP draws:     ", self->ffpDraws);
        log_int("    Skinned draws: ", self->skinnedDraws);
        log_int("    Quad skips:    ", self->quadSkips);
        log_int("    Float3 passes: ", self->float3Passes);
        log_int("    Shader draws:  ", self->shaderDraws);
        log_int("    Decomposed:    ", self->decomposedCount);
        log_int("    Fallback:      ", self->fallbackCount);
        {
            float *gameProj = (float *)TRL_PROJ_ADDR;
            float *gameView = (float *)TRL_VIEW_ADDR;
            diag_log_matrix("  gameProj(mem)", gameProj);
            diag_log_matrix("  gameView(mem)", gameView);
        }
        self->diagLoggedFrames++;
        { int ts; for (ts = 0; ts < 8; ts++) self->diagTexUniq[ts] = 0; }
    }
#endif

    self->frameCount++;
    self->ffpSetup = 0;
    self->drawCallCount = 0;
    self->sceneCount = 0;
    self->ffpDraws = 0;
    self->skinnedDraws = 0;
    self->quadSkips = 0;
    self->float3Passes = 0;
    self->shaderDraws = 0;
    self->decomposedCount = 0;
    self->fallbackCount = 0;
    FFP_Disengage(self);
    {
        int r;
        for (r = 0; r < 256; r++) self->vsConstWriteLog[r] = 0;
    }
    hr = ((FN)RealVtbl(self)[SLOT_Present])(self->pReal, a, b, c, d);

    return hr;
}

/* 41: BeginScene */
static int __stdcall WD_BeginScene(WrappedDevice *self) {
    typedef int (__stdcall *FN)(void*);
    self->ffpSetup = 0;
    self->sceneCount++;
    return ((FN)RealVtbl(self)[SLOT_BeginScene])(self->pReal);
}

/* 42: EndScene */
static int __stdcall WD_EndScene(WrappedDevice *self) {
    typedef int (__stdcall *FN)(void*);
    return ((FN)RealVtbl(self)[SLOT_EndScene])(self->pReal);
}

/*
 * 44: SetTransform — Intercept to prevent dxwrapper overrides during FFP.
 *
 * dxwrapper's D3D8-to-D3D9 conversion calls SetTransform with
 * View=identity, Proj=identity, World=WVP. If these reach the real
 * device they overwrite the proxy's decomposed transforms between draws.
 * Block all SetTransform calls while FFP is active; relay otherwise.
 */
static int __stdcall WD_SetTransform(WrappedDevice *self, unsigned int state, float *pMatrix) {
    typedef int (__stdcall *FN)(void*, unsigned int, float*);
    if (self->ffpActive) return 0;
    return ((FN)RealVtbl(self)[SLOT_SetTransform])(self->pReal, state, pMatrix);
}

/* 49: SetMaterial — relay through */
static int __stdcall WD_SetMaterial(WrappedDevice *self, void *pMaterial) {
    typedef int (__stdcall *FN)(void*, void*);
    return ((FN)RealVtbl(self)[SLOT_SetMaterial])(self->pReal, pMaterial);
}

/* 57: SetRenderState — Force D3DCULL_NONE and D3DFILL_SOLID globally */
static int __stdcall WD_SetRenderState(WrappedDevice *self, unsigned int state, unsigned int value) {
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int);

    /* Force no backface culling on ALL draws */
    if (state == D3DRS_CULLMODE) {
        return ((FN)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, state, 1); /* D3DCULL_NONE */
    }

    /* Force solid fill — prevent any wireframe rendering */
    if (state == D3DRS_FILLMODE) {
        return ((FN)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, state, D3DFILL_SOLID);
    }

    return ((FN)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, state, value);
}

/* 67: SetTextureStageState — relay through */
static int __stdcall WD_SetTextureStageState(WrappedDevice *self,
    unsigned int stage, unsigned int type, unsigned int value) {
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, unsigned int);
    return ((FN)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, stage, type, value);
}

/*
 * 81: DrawPrimitive
 *
 * GAME-SPECIFIC: In many engines, DrawPrimitive is used for particles,
 * UI, and other elements that need shader-based rendering. The default
 * behavior is to disengage FFP and pass through with original shaders.
 *
 * If your game uses DP for world geometry too, you may need to add
 * FFP conversion logic here similar to DrawIndexedPrimitive.
 */
static int __stdcall WD_DrawPrimitive(WrappedDevice *self, unsigned int pt, unsigned int sv, unsigned int pc) {
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_RS)(void*, unsigned int, unsigned int);
    int hr;
    self->drawCallCount++;

    /* Suppress line primitives (allow points for particles) */
    if (pt == 2 || pt == 3) return 0;

    /* Force solid fill before every draw — overrides any wireframe state
     * set by dxwrapper or state blocks bypassing our SetRenderState intercept */
    ((FN_RS)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, D3DRS_FILLMODE, D3DFILL_SOLID);

    FFP_Disengage(self);
    hr = ((FN)RealVtbl(self)[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);

#if DIAG_ENABLED
    if (DIAG_ACTIVE(self) && self->drawCallCount <= 200) {
        log_int("  DP #", self->drawCallCount);
        log_int("    type=", pt);
        log_int("    startVtx=", sv);
        log_int("    primCount=", pc);
        log_hex("    hr=", hr);
        if (self->drawCallCount <= 5) {
            diag_log_matrix("    wvp(c0)", &self->vsConst[VS_REG_WVP_START * 4]);
            diag_log_matrix("    viewProj(c12)", &self->vsConst[VS_REG_VIEWPROJ_START * 4]);
        }
    }
#endif
    return hr;
}

/*
 * 82: DrawIndexedPrimitive — Core FFP conversion
 *
 * GAME-SPECIFIC: This is the main decision point. The routing logic
 * determines which draws get FFP-converted vs passed through with shaders.
 *
 * Default behavior:
 *   - If view/proj not yet captured -> pass through with shaders
 *   - If skinned mesh -> pass through with shaders
 *   - Otherwise -> engage FFP, draw
 *
 * You may need to adjust this for your game:
 *   - Some games need shader pass-through for specific draw calls
 *     (shadow maps, post-processing, etc.) — filter by shader pointer,
 *     render target, vertex count, or other state
 *   - Some games use different texcoord formats or vertex layouts
 */
static int __stdcall WD_DrawIndexedPrimitive(WrappedDevice *self,
    unsigned int pt, int bvi, unsigned int mi, unsigned int nv,
    unsigned int si, unsigned int pc)
{
    typedef int (__stdcall *FN)(void*, unsigned int, int, unsigned int, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_RS)(void*, unsigned int, unsigned int);
    int hr;
    self->drawCallCount++;

    /* Suppress line primitives (allow points for particles) */
    if (pt == 2 || pt == 3) return 0;

    /* Force solid fill before every draw */
    ((FN_RS)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, D3DRS_FILLMODE, D3DFILL_SOLID);

    if (self->wvpValid && pt == 4 && self->streamStride[0] >= 12) {

        /* FLOAT3 position draws: characters (Lara) and screen-space effects.
         * TRL uses GPU skinning via VS constants, not BLENDWEIGHT in vertex decl,
         * so curDeclIsSkinned is always 0. Distinguish by comparing WVP to Proj:
         *   WVP ≈ Proj → screen-space (post-process, UI) → skip
         *   WVP ≠ Proj → real geometry (Lara, characters) → decompose + draw */
        if (self->curDeclPosIsFloat3) {
            float wvpCheck[16];
            float *gP = (float *)TRL_PROJ_ADDR;
            int isScreenSpace = 1;
            int pi;
            mat4_transpose(wvpCheck, &self->vsConst[VS_REG_WVP_START * 4]);
            for (pi = 0; pi < 16; pi++) {
                float diff = wvpCheck[pi] - gP[pi];
                if (diff > 0.05f || diff < -0.05f) { isScreenSpace = 0; break; }
            }
            if (isScreenSpace) {
                self->quadSkips++;
                return 0;
            }
            /* FLOAT3 character geometry: decompose W/V/P and draw with shaders */
            FFP_Engage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
            self->skinnedDraws++;
        } else if (self->curDeclIsSkinned) {
            /* Skinned mesh with BLENDWEIGHT (not used by TRL, but kept for safety) */
            {
                typedef int (__stdcall *FN_ST)(void*, unsigned int, float*);
                void **vt = RealVtbl(self);
                float *gV = (float *)TRL_VIEW_ADDR;
                float *gP = (float *)TRL_PROJ_ADDR;
                ((FN_ST)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD, (float*)s_identity);
                ((FN_ST)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, gV);
                ((FN_ST)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, gP);
                self->ffpActive = 1;
            }
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
            self->skinnedDraws++;
        } else {
            /* Rigid mesh: decompose W/V/P from WVP constants + game memory */
            self->ffpDraws++;
            FFP_Engage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        }
    } else {
        /* Non-triangle, no WVP, or tiny stride — passthrough */
        hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        self->shaderDraws++;
    }

#if DIAG_ENABLED
    /* Track unique textures per stage */
    if (DIAG_ACTIVE(self)) {
        int ts;
        for (ts = 0; ts < 8; ts++) {
            if (self->curTexture[ts]) {
                int found = 0, k;
                for (k = 0; k < self->diagTexUniq[ts] && k < 32; k++) {
                    if (self->diagTexSeen[ts][k] == self->curTexture[ts]) { found = 1; break; }
                }
                if (!found && self->diagTexUniq[ts] < 32) {
                    self->diagTexSeen[ts][self->diagTexUniq[ts]] = self->curTexture[ts];
                    self->diagTexUniq[ts]++;
                }
            }
        }
    }
    if (DIAG_ACTIVE(self) && self->diagDipLogged < 200) {
        /* Determine route taken for this draw */
        const char *route = "SHADER";
        if (self->wvpValid && pt == 4 && self->streamStride[0] >= 12) {
            if (pc <= 2 && nv <= 6 && !self->curDeclHasNormal) route = "QUAD_SKIP";
            else if (self->curDeclIsSkinned)                    route = "SKINNED";
            else                                                route = "FFP";
        }

        log_int("  DIP #", self->drawCallCount);
        log_str("    route="); log_str(route); log_str("\r\n");
        log_int("    numVerts=", nv);
        log_int("    primCount=", pc);
        log_int("    stride0=", self->streamStride[0]);
        log_int("    hasTexcoord=", self->curDeclHasTexcoord);
        log_int("    wvpValid=", self->wvpValid);
        log_int("    ffpActive=", self->ffpActive);
        log_hex("    vs=", (unsigned int)self->lastVS);
        log_hex("    ps=", (unsigned int)self->lastPS);
        {
            int ts;
            for (ts = 0; ts < 4; ts++) {
                if (self->curTexture[ts]) {
                    log_int("    tex", ts);
                    log_hex("     =", (unsigned int)self->curTexture[ts]);
                }
            }
        }
        /* Log transforms for first 20 draws so we can see decomposition results */
        if (self->diagDipLogged < 20) {
            float *gameProj = (float *)TRL_PROJ_ADDR;
            diag_log_matrix("    wvp(c0)", &self->vsConst[VS_REG_WVP_START * 4]);
            diag_log_matrix("    gameProj", gameProj);
        }
        self->diagDipLogged++;
    }
#endif
    return hr;
}

/* 83: DrawPrimitiveUP — suppress line primitives, force solid fill */
static int __stdcall WD_DrawPrimitiveUP(WrappedDevice *self, unsigned int pt,
    unsigned int pc, void *pData, unsigned int stride)
{
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, void*, unsigned int);
    typedef int (__stdcall *FN_RS)(void*, unsigned int, unsigned int);
    if (pt == 2 || pt == 3) return 0;
    ((FN_RS)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, D3DRS_FILLMODE, D3DFILL_SOLID);
    return ((FN)RealVtbl(self)[SLOT_DrawPrimitiveUP])(self->pReal, pt, pc, pData, stride);
}

/* 84: DrawIndexedPrimitiveUP — suppress line primitives, force solid fill */
static int __stdcall WD_DrawIndexedPrimitiveUP(WrappedDevice *self, unsigned int pt,
    unsigned int minIdx, unsigned int numVerts, unsigned int primCount,
    void *pIndexData, unsigned int idxFmt, void *pVertData, unsigned int stride)
{
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, unsigned int, unsigned int, void*, unsigned int, void*, unsigned int);
    typedef int (__stdcall *FN_RS)(void*, unsigned int, unsigned int);
    if (pt == 2 || pt == 3) return 0;
    ((FN_RS)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, D3DRS_FILLMODE, D3DFILL_SOLID);
    return ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitiveUP])(self->pReal, pt, minIdx, numVerts, primCount, pIndexData, idxFmt, pVertData, stride);
}

/* 92: SetVertexShader */
static int __stdcall WD_SetVertexShader(WrappedDevice *self, void *pShader) {
    typedef int (__stdcall *FN)(void*, void*);
#if DIAG_ENABLED
    if (DIAG_ACTIVE(self)) {
        log_hex("  SetVS shader=", (unsigned int)pShader);
    }
#endif
    shader_addref(pShader);
    shader_release(self->lastVS);
    self->lastVS = pShader;
    self->ffpActive = 0;
    return ((FN)RealVtbl(self)[SLOT_SetVertexShader])(self->pReal, pShader);
}

/*
 * 94: SetVertexShaderConstantF — Capture constants for FFP transform mapping
 *
 * TRL uploads matrices in bulk (count=8 covering two 4x4 matrices).
 * We accept all writes and track dirty flags per register range.
 */
static int __stdcall WD_SetVertexShaderConstantF(WrappedDevice *self,
    unsigned int startReg, float *pData, unsigned int count)
{
    typedef int (__stdcall *FN)(void*, unsigned int, float*, unsigned int);
    unsigned int i;
    unsigned int endReg = startReg + count;

    if (pData && startReg + count <= 256) {
        for (i = 0; i < count * 4; i++) {
            self->vsConst[(startReg * 4) + i] = pData[i];
        }

        /* Dirty tracking per register range */
        if (startReg < VS_REG_WVP_END && endReg > VS_REG_WVP_START) {
            self->wvpDirty = 1;
            /* Only mark valid if non-zero */
            {
                float *wvp = &self->vsConst[VS_REG_WVP_START * 4];
                int nonzero = 0;
                unsigned int wi;
                for (wi = 0; wi < 16; wi++) {
                    if (wvp[wi] != 0.0f) { nonzero = 1; break; }
                }
                if (nonzero) self->wvpValid = 1;
            }
        }
        if (startReg < VS_REG_WORLD_END && endReg > VS_REG_WORLD_START) {
            self->worldDirty = 1;
        }
        if (startReg < VS_REG_VIEWPROJ_END && endReg > VS_REG_VIEWPROJ_START) {
            self->viewProjDirty = 1;
            /* Only mark valid if the ViewProj data is non-zero — the game
             * uploads c8 count=8 with zeros in the c12-c15 range during menus. */
            {
                float *vp = &self->vsConst[VS_REG_VIEWPROJ_START * 4];
                int nonzero = 0;
                unsigned int vi;
                for (vi = 0; vi < 16; vi++) {
                    if (vp[vi] != 0.0f) { nonzero = 1; break; }
                }
                if (nonzero) self->viewProjValid = 1;
            }
        }

        for (i = 0; i < count; i++) {
            unsigned int reg = startReg + i;
            if (reg < 256) self->vsConstWriteLog[reg] = 1;
        }

#if ENABLE_SKINNING
        if (startReg >= VS_REG_BONE_THRESHOLD &&
            count >= VS_BONE_MIN_REGS &&
            (count % VS_REGS_PER_BONE) == 0) {
            self->boneStartReg = startReg;
            self->numBones = count / VS_REGS_PER_BONE;
        }
#endif

#if DIAG_ENABLED
        if (DIAG_ACTIVE(self) &&
            self->diagVsLogged < 120 &&
            (startReg <= VS_REG_VIEWPROJ_END) &&
            (count >= 2)) {
            log_int("  SetVSConstF start=", startReg);
            log_int("    count=", count);
            log_int("    viewProjValid=", self->viewProjValid);
            log_int("    wvpValid=", self->wvpValid);
            if (count == 4) {
                diag_log_matrix("    mat", pData);
            } else if (count == 8) {
                diag_log_matrix("    mat0", pData);
                diag_log_matrix("    mat1", pData + 16);
            } else if (count <= 3) {
                log_floats_dec("    data: ", pData, count * 4);
            }
            self->diagVsLogged++;
        }
#endif
    }

    return ((FN)RealVtbl(self)[SLOT_SetVertexShaderConstantF])(self->pReal, startReg, pData, count);
}

/* 107: SetPixelShader — always forward; shaders stay active in this proxy */
static int __stdcall WD_SetPixelShader(WrappedDevice *self, void *pShader) {
    typedef int (__stdcall *FN)(void*, void*);
    shader_addref(pShader);
    shader_release(self->lastPS);
    self->lastPS = pShader;
    return ((FN)RealVtbl(self)[SLOT_SetPixelShader])(self->pReal, pShader);
}

/* 109: SetPixelShaderConstantF */
static int __stdcall WD_SetPixelShaderConstantF(WrappedDevice *self,
    unsigned int startReg, float *pData, unsigned int count)
{
    typedef int (__stdcall *FN)(void*, unsigned int, float*, unsigned int);
    unsigned int i;
    if (pData && startReg + count <= 32) {
        for (i = 0; i < count * 4; i++) {
            self->psConst[(startReg * 4) + i] = pData[i];
        }
        self->psConstDirty = 1;
    }
    return ((FN)RealVtbl(self)[SLOT_SetPixelShaderConstantF])(self->pReal, startReg, pData, count);
}

/* 65: SetTexture */
static int __stdcall WD_SetTexture(WrappedDevice *self, unsigned int stage, void *pTexture) {
    typedef int (__stdcall *FN)(void*, unsigned int, void*);
    if (stage < 8) {
        self->curTexture[stage] = pTexture;
    }
    return ((FN)RealVtbl(self)[SLOT_SetTexture])(self->pReal, stage, pTexture);
}

/* 100: SetStreamSource */
static int __stdcall WD_SetStreamSource(WrappedDevice *self,
    unsigned int stream, void *pVB, unsigned int offset, unsigned int stride)
{
    typedef int (__stdcall *FN)(void*, unsigned int, void*, unsigned int, unsigned int);
    if (stream < 4) {
        self->streamVB[stream] = pVB;
        self->streamOffset[stream] = offset;
        self->streamStride[stream] = stride;
    }
    return ((FN)RealVtbl(self)[SLOT_SetStreamSource])(self->pReal, stream, pVB, offset, stride);
}

/*
 * 87: SetVertexDeclaration — Parse vertex elements, detect skinning
 *
 * Parses the raw D3DVERTEXELEMENT9 array to detect:
 *   - BLENDWEIGHT + BLENDINDICES -> skinned mesh
 *   - TEXCOORD[0] presence
 *   - Presence of NORMAL, COLOR
 */
static int __stdcall WD_SetVertexDeclaration(WrappedDevice *self, void *pDecl) {
    typedef int (__stdcall *FN)(void*, void*);

    self->lastDecl = pDecl;
    self->curDeclIsSkinned = 0;
#if ENABLE_SKINNING
    self->curDeclNumWeights = 0;
#endif
    self->curDeclHasTexcoord = 0;
    self->curDeclHasNormal = 0;
    self->curDeclHasColor = 0;
    self->curDeclPosIsFloat3 = 0;
    self->curDeclNumElems = 0;

    if (pDecl) {
        typedef int (__stdcall *FN_GetDecl)(void*, void*, unsigned int*);
        void **declVt = *(void***)pDecl;
        unsigned char elemBuf[8 * 32];
        unsigned int numElems = 0;
        int hr2 = ((FN_GetDecl)declVt[4])(pDecl, NULL, &numElems);
        if (hr2 == 0 && numElems > 0 && numElems <= 32) {
            hr2 = ((FN_GetDecl)declVt[4])(pDecl, elemBuf, &numElems);
            if (hr2 == 0) {
                unsigned int e;
                int hasBlendWeight = 0, hasBlendIndices = 0;
                int blendWeightType = 0;
                int realElems = 0;

                for (e = 0; e < numElems; e++) {
                    unsigned char *el = &elemBuf[e * 8];
                    unsigned short stream = *(unsigned short*)&el[0];
                    unsigned char usage = el[6];
                    unsigned char usageIdx = el[7];
                    unsigned char type = el[4];
                    if (stream == 0xFF || stream == 0xFFFF) break;
                    realElems++;
                    if (usage == D3DDECLUSAGE_POSITION && usageIdx == 0) {
                        self->curDeclPosIsFloat3 = (type == D3DDECLTYPE_FLOAT3);
                    }
                    if (usage == D3DDECLUSAGE_BLENDWEIGHT) {
                        hasBlendWeight = 1;
                        blendWeightType = type;
                    }
                    if (usage == D3DDECLUSAGE_BLENDINDICES) {
                        hasBlendIndices = 1;
                    }
                    if (usage == D3DDECLUSAGE_TEXCOORD && usageIdx == 0 && stream == 0) {
                        self->curDeclHasTexcoord = 1;
                    }
                    if (usage == D3DDECLUSAGE_NORMAL) {
                        self->curDeclHasNormal = 1;
                    }
                    if (usage == D3DDECLUSAGE_COLOR) {
                        self->curDeclHasColor = 1;
                    }
                }

                if (realElems <= 20) {
                    int i;
                    self->curDeclNumElems = realElems;
                    for (i = 0; i < realElems * 8; i++)
                        self->curDeclElems[i] = elemBuf[i];
                }

                if (hasBlendWeight && hasBlendIndices) {
                    self->curDeclIsSkinned = 1;
#if ENABLE_SKINNING
                    switch (blendWeightType) {
                        case 0: self->curDeclNumWeights = 1; break;
                        case 1: self->curDeclNumWeights = 2; break;
                        case 2: self->curDeclNumWeights = 3; break;
                        case 3: self->curDeclNumWeights = 3; break;
                        default: self->curDeclNumWeights = 3; break;
                    }
#endif
                }

#if DIAG_ENABLED
                if (DIAG_ACTIVE(self)) {
                    int alreadyLogged = 0, di;
                    for (di = 0; di < self->loggedDeclCount; di++) {
                        if (self->loggedDecls[di] == pDecl) { alreadyLogged = 1; break; }
                    }
                    if (!alreadyLogged && self->loggedDeclCount < 32) {
                        static const char *usageNames[] = {
                            "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",
                            "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
                            "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"
                        };
                        static const char *typeNames[] = {
                            "FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR",
                            "UBYTE4", "SHORT2", "SHORT4", "UBYTE4N", "SHORT2N",
                            "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N",
                            "FLOAT16_2", "FLOAT16_4", "UNUSED"
                        };
                        self->loggedDecls[self->loggedDeclCount++] = pDecl;
                        log_hex("  DECL decl=", (unsigned int)pDecl);
                        log_int("    numElems=", numElems);
                        if (self->curDeclIsSkinned) {
                            log_str("    SKINNED\r\n");
                        }
                        for (e = 0; e < numElems; e++) {
                            unsigned char *el = &elemBuf[e * 8];
                            unsigned short stream = *(unsigned short*)&el[0];
                            unsigned short offset = *(unsigned short*)&el[2];
                            unsigned char type = el[4];
                            unsigned char usage = el[6];
                            unsigned char usageIdx = el[7];
                            if (stream == 0xFF || stream == 0xFFFF) break;
                            log_str("    [s");
                            log_int("", stream);
                            log_str("    +");
                            log_int("", offset);
                            log_str("    ] ");
                            if (usage < 14) log_str(usageNames[usage]);
                            else log_int("usage=", usage);
                            log_str("[");
                            {
                                char ub[4]; ub[0] = '0' + usageIdx; ub[1] = ']'; ub[2] = ' '; ub[3] = 0;
                                log_str(ub);
                            }
                            if (type <= 17) log_str(typeNames[type]);
                            else log_int("type=", type);
                            log_str("\r\n");
                        }
                    }
                }
#endif
            }
        }
    }

    return ((FN)RealVtbl(self)[SLOT_SetVertexDeclaration])(self->pReal, pDecl);
}

/* ---- Build vtable ---- */

/*
 * In-memory patches for TRL: disable frustum culling and cull-mode check.
 * Called once at device creation.
 */
/* Helper: patch a single float in the game's memory */
static int TRL_PatchFloat(unsigned int addr, float value, const char *name) {
    unsigned long oldProtect;
    float *target = (float *)addr;
    if (VirtualProtect(target, sizeof(float), 0x04 /*PAGE_READWRITE*/, &oldProtect)) {
        *target = value;
        VirtualProtect(target, sizeof(float), oldProtect, &oldProtect);
        log_str(name);
        log_str(" patched\r\n");
        return 1;
    }
    return 0;
}

/* Enforce view distance cap every frame (game's camera setup overwrites it).
 * Use a large but finite value — 1e30 causes the spatial tree to traverse
 * the entire level, freezing the game. 100000 is ~8x the default far clip
 * (12288) which covers all visible geometry without rendering the universe. */
static void TRL_EnforceViewDistance(void) {
    float *viewDist = (float *)TRL_VIEW_DIST_ADDR;
    *viewDist = 100000.0f;
}

static void TRL_ApplyGamePatches(void) {
    unsigned long oldProtect;

    /* 1. Frustum distance threshold — must be large but finite.
     * 1e30 causes the spatial tree to traverse the entire level, freezing.
     * 100000 is ~8x default far clip (12288), covers all visible geometry. */
    TRL_PatchFloat(TRL_FRUSTUM_THRESH, 100000.0f, "Frustum threshold");

    /* 2. Cull-mode conditional: always set ECX=1 (render everything) */
    {
        static const unsigned char expected[TRL_CULL_PATCH_LEN] = {
            0xF7,0xC3,0x00,0x00,0x20,0x00,0xB9,0x00,0x00,0x00,0x00,0x0F,0x95,0xC1,0x41
        };
        static const unsigned char patched[TRL_CULL_PATCH_LEN] = {
            0xB9,0x01,0x00,0x00,0x00,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
        };
        unsigned char *site = (unsigned char *)TRL_CULL_PATCH_VA;

        if (VirtualProtect(site, TRL_CULL_PATCH_LEN, 0x40 /*PAGE_EXECUTE_READWRITE*/, &oldProtect)) {
            int i, isOrig = 1, isPatched = 1;
            for (i = 0; i < TRL_CULL_PATCH_LEN; i++) {
                if (site[i] != expected[i]) isOrig = 0;
                if (site[i] != patched[i]) isPatched = 0;
            }
            if (isOrig) {
                for (i = 0; i < TRL_CULL_PATCH_LEN; i++) site[i] = patched[i];
                log_str("Cull mode patch applied\r\n");
            } else if (isPatched) {
                log_str("Cull mode patch already applied\r\n");
            }
            VirtualProtect(site, TRL_CULL_PATCH_LEN, oldProtect, &oldProtect);
        }
    }

    /* 3. Spatial tree view distance — large but finite to avoid freeze */
    TRL_EnforceViewDistance();
    log_str("View distance enforced (100000)\r\n");

    /* 4. Far clipping plane — extend moderately (was 12288) */
    TRL_PatchFloat(TRL_FAR_CLIP_ADDR, 100000.0f, "Far clip plane");
}

WrappedDevice* WrappedDevice_Create(void *pRealDevice) {
    WrappedDevice *w;

    w = (WrappedDevice*)HeapAlloc(GetProcessHeap(), 8 /*HEAP_ZERO_MEMORY*/, sizeof(WrappedDevice));
    if (!w) return NULL;

    s_device_vtbl[0]  = (void*)WD_QueryInterface;
    s_device_vtbl[1]  = (void*)WD_AddRef;
    s_device_vtbl[2]  = (void*)WD_Release;
    s_device_vtbl[3]  = (void*)Relay_03;
    s_device_vtbl[4]  = (void*)Relay_04;
    s_device_vtbl[5]  = (void*)Relay_05;
    s_device_vtbl[6]  = (void*)Relay_06;
    s_device_vtbl[7]  = (void*)Relay_07;
    s_device_vtbl[8]  = (void*)Relay_08;
    s_device_vtbl[9]  = (void*)Relay_09;
    s_device_vtbl[10] = (void*)Relay_10;
    s_device_vtbl[11] = (void*)Relay_11;
    s_device_vtbl[12] = (void*)Relay_12;
    s_device_vtbl[13] = (void*)Relay_13;
    s_device_vtbl[14] = (void*)Relay_14;
    s_device_vtbl[15] = (void*)Relay_15;
    s_device_vtbl[16] = (void*)WD_Reset;             /* INTERCEPTED */
    s_device_vtbl[17] = (void*)WD_Present;           /* INTERCEPTED */
    s_device_vtbl[18] = (void*)Relay_18;
    s_device_vtbl[19] = (void*)Relay_19;
    s_device_vtbl[20] = (void*)Relay_20;
    s_device_vtbl[21] = (void*)Relay_21;
    s_device_vtbl[22] = (void*)Relay_22;
    s_device_vtbl[23] = (void*)Relay_23;
    s_device_vtbl[24] = (void*)Relay_24;
    s_device_vtbl[25] = (void*)Relay_25;
    s_device_vtbl[26] = (void*)Relay_26;
    s_device_vtbl[27] = (void*)Relay_27;
    s_device_vtbl[28] = (void*)Relay_28;
    s_device_vtbl[29] = (void*)Relay_29;
    s_device_vtbl[30] = (void*)Relay_30;
    s_device_vtbl[31] = (void*)Relay_31;
    s_device_vtbl[32] = (void*)Relay_32;
    s_device_vtbl[33] = (void*)Relay_33;
    s_device_vtbl[34] = (void*)Relay_34;
    s_device_vtbl[35] = (void*)Relay_35;
    s_device_vtbl[36] = (void*)Relay_36;
    s_device_vtbl[37] = (void*)Relay_37;
    s_device_vtbl[38] = (void*)Relay_38;
    s_device_vtbl[39] = (void*)Relay_39;
    s_device_vtbl[40] = (void*)Relay_40;
    s_device_vtbl[41] = (void*)WD_BeginScene;        /* INTERCEPTED */
    s_device_vtbl[42] = (void*)WD_EndScene;           /* INTERCEPTED */
    s_device_vtbl[43] = (void*)Relay_43;
    s_device_vtbl[44] = (void*)WD_SetTransform;   /* INTERCEPTED */
    s_device_vtbl[45] = (void*)Relay_45;
    s_device_vtbl[46] = (void*)Relay_46;
    s_device_vtbl[47] = (void*)Relay_47;
    s_device_vtbl[48] = (void*)Relay_48;
    s_device_vtbl[49] = (void*)WD_SetMaterial;    /* INTERCEPTED */
    s_device_vtbl[50] = (void*)Relay_50;
    s_device_vtbl[51] = (void*)Relay_51;
    s_device_vtbl[52] = (void*)Relay_52;
    s_device_vtbl[53] = (void*)Relay_53;
    s_device_vtbl[54] = (void*)Relay_54;
    s_device_vtbl[55] = (void*)Relay_55;
    s_device_vtbl[56] = (void*)Relay_56;
    s_device_vtbl[57] = (void*)WD_SetRenderState; /* INTERCEPTED */
    s_device_vtbl[58] = (void*)Relay_58;
    s_device_vtbl[59] = (void*)Relay_59;
    s_device_vtbl[60] = (void*)Relay_60;
    s_device_vtbl[61] = (void*)Relay_61;
    s_device_vtbl[62] = (void*)Relay_62;
    s_device_vtbl[63] = (void*)Relay_63;
    s_device_vtbl[64] = (void*)Relay_64;
    s_device_vtbl[65] = (void*)WD_SetTexture;        /* INTERCEPTED */
    s_device_vtbl[66] = (void*)Relay_66;
    s_device_vtbl[67] = (void*)WD_SetTextureStageState; /* INTERCEPTED */
    s_device_vtbl[68] = (void*)Relay_68;
    s_device_vtbl[69] = (void*)Relay_69;
    s_device_vtbl[70] = (void*)Relay_70;
    s_device_vtbl[71] = (void*)Relay_71;
    s_device_vtbl[72] = (void*)Relay_72;
    s_device_vtbl[73] = (void*)Relay_73;
    s_device_vtbl[74] = (void*)Relay_74;
    s_device_vtbl[75] = (void*)Relay_75;
    s_device_vtbl[76] = (void*)Relay_76;
    s_device_vtbl[77] = (void*)Relay_77;
    s_device_vtbl[78] = (void*)Relay_78;
    s_device_vtbl[79] = (void*)Relay_79;
    s_device_vtbl[80] = (void*)Relay_80;
    s_device_vtbl[81] = (void*)WD_DrawPrimitive;     /* INTERCEPTED */
    s_device_vtbl[82] = (void*)WD_DrawIndexedPrimitive; /* INTERCEPTED */
    s_device_vtbl[83] = (void*)WD_DrawPrimitiveUP;     /* INTERCEPTED */
    s_device_vtbl[84] = (void*)WD_DrawIndexedPrimitiveUP; /* INTERCEPTED */
    s_device_vtbl[85] = (void*)Relay_85;
    s_device_vtbl[86] = (void*)Relay_86;
    s_device_vtbl[87] = (void*)WD_SetVertexDeclaration; /* INTERCEPTED */
    s_device_vtbl[88] = (void*)Relay_88;
    s_device_vtbl[89] = (void*)Relay_89;
    s_device_vtbl[90] = (void*)Relay_90;
    s_device_vtbl[91] = (void*)Relay_91;
    s_device_vtbl[92] = (void*)WD_SetVertexShader;   /* INTERCEPTED */
    s_device_vtbl[93] = (void*)Relay_93;
    s_device_vtbl[94] = (void*)WD_SetVertexShaderConstantF; /* INTERCEPTED */
    s_device_vtbl[95] = (void*)Relay_95;
    s_device_vtbl[96] = (void*)Relay_96;
    s_device_vtbl[97] = (void*)Relay_97;
    s_device_vtbl[98] = (void*)Relay_98;
    s_device_vtbl[99] = (void*)Relay_99;
    s_device_vtbl[100] = (void*)WD_SetStreamSource;  /* INTERCEPTED */
    s_device_vtbl[101] = (void*)Relay_101;
    s_device_vtbl[102] = (void*)Relay_102;
    s_device_vtbl[103] = (void*)Relay_103;
    s_device_vtbl[104] = (void*)Relay_104;
    s_device_vtbl[105] = (void*)Relay_105;
    s_device_vtbl[106] = (void*)Relay_106;
    s_device_vtbl[107] = (void*)WD_SetPixelShader;   /* INTERCEPTED */
    s_device_vtbl[108] = (void*)Relay_108;
    s_device_vtbl[109] = (void*)WD_SetPixelShaderConstantF; /* INTERCEPTED */
    s_device_vtbl[110] = (void*)Relay_110;
    s_device_vtbl[111] = (void*)Relay_111;
    s_device_vtbl[112] = (void*)Relay_112;
    s_device_vtbl[113] = (void*)Relay_113;
    s_device_vtbl[114] = (void*)Relay_114;
    s_device_vtbl[115] = (void*)Relay_115;
    s_device_vtbl[116] = (void*)Relay_116;
    s_device_vtbl[117] = (void*)Relay_117;
    s_device_vtbl[118] = (void*)Relay_118;

    w->vtbl = s_device_vtbl;
    w->pReal = pRealDevice;
    w->refCount = 1;
    w->frameCount = 0;
    w->ffpSetup = 0;
    w->worldDirty = 0;
    w->viewProjDirty = 0;
    w->wvpDirty = 0;
    w->psConstDirty = 0;
    w->lastVS = NULL;
    w->lastPS = NULL;
    w->viewProjValid = 0;
    w->wvpValid = 0;
    w->lastDecl = NULL;
    w->curDeclIsSkinned = 0;
#if ENABLE_SKINNING
    w->curDeclNumWeights = 0;
    w->boneStartReg = 0;
    w->numBones = 0;
    w->skinningSetup = 0;
    w->lastVsConstWriteEnd = 0;
#endif
    { int s; for (s = 0; s < 4; s++) { w->streamVB[s] = NULL; w->streamOffset[s] = 0; w->streamStride[s] = 0; } }
    { int t; for (t = 0; t < 8; t++) w->curTexture[t] = NULL; }
    w->loggedDeclCount = 0;
    { int ts; for (ts = 0; ts < 8; ts++) w->diagTexUniq[ts] = 0; }
    w->createTick = GetTickCount();
    w->diagLoggedFrames = 0;
    w->diagVsLogged = 0;
    w->diagDipLogged = 0;

    /* Read AlbedoStage from INI */
    {
        char iniBuf[260];
        extern HINSTANCE g_hInstance;
        int i, lastSlash = -1;
        GetModuleFileNameA(g_hInstance, iniBuf, 260);
        for (i = 0; iniBuf[i]; i++) {
            if (iniBuf[i] == '\\' || iniBuf[i] == '/') lastSlash = i;
        }
        if (lastSlash >= 0) {
            const char *fn = "proxy.ini";
            int p = lastSlash + 1, j;
            for (j = 0; fn[j]; j++) iniBuf[p++] = fn[j];
            iniBuf[p] = '\0';
        }
        w->albedoStage = GetPrivateProfileIntA("FFP", "AlbedoStage", 0, iniBuf);
        if (w->albedoStage < 0 || w->albedoStage > 7) w->albedoStage = 0;
        w->disableNormalMaps = GetPrivateProfileIntA("FFP", "DisableNormalMaps", 1, iniBuf) ? 1 : 0;
    }

    /* Game patches disabled — frustum/cull/view-distance overrides cause the
     * spatial tree to process too many nodes, freezing the game on level load.
     * The shader-passthrough + transform-override approach works without them. */
    /* TRL_ApplyGamePatches(); */

    log_str("WrappedDevice created with FFP conversion\r\n");
    log_int("  Diag delay (ms): ", DIAG_DELAY_MS);
    log_int("  AlbedoStage: ", w->albedoStage);
    log_int("  DisableNormalMaps: ", w->disableNormalMaps);
    log_hex("  Real device: ", (unsigned int)pRealDevice);
    log_hex("  Wrapped device: ", (unsigned int)w);
    return w;
}
