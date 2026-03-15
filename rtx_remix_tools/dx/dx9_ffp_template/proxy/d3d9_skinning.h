/*
 * d3d9_skinning.h — FFP Indexed Vertex Blending Extension
 *
 * This file is #included by d3d9_device.c ONLY when ENABLE_SKINNING=1.
 * It contains all skinning infrastructure for converting skinned meshes
 * (BLENDWEIGHT + BLENDINDICES vertex declarations) to FFP indexed
 * vertex blending with bone matrices.
 *
 * THIS IS A LATE-STAGE EXTENSION. It should only be enabled after:
 *   - Basic FFP conversion works (rigid geometry renders correctly)
 *   - Matrix register mapping is confirmed
 *   - AlbedoStage and texture routing are correct
 *
 * To enable: set #define ENABLE_SKINNING 1 in d3d9_device.c
 *
 * Contents:
 *   - half_to_float          — IEEE 754 binary16 → binary32 decoder
 *   - decode_normal           — compressed normal format decoder
 *   - expand_skin_vertex      — per-vertex expansion to fixed 48-byte layout
 *   - SkinVB_GetExpanded      — vertex buffer expansion with caching
 *   - SkinVB_ReleaseCache     — free cached expanded VBs
 *   - FFP_UploadBones         — upload bone matrices via SetTransform
 *   - FFP_DisableSkinning     — reset indexed vertex blending state
 *   - Skin_DrawDIP            — complete skinned DrawIndexedPrimitive handler
 *   - Skin_InitDevice         — create skinExpDecl at device creation
 *   - Skin_ReleaseDevice      — free VB cache + skinExpDecl on device release
 *
 * All functions are static and require the WrappedDevice struct, slot
 * enums, and D3D9 constants from d3d9_device.c to be defined first.
 */

/* Forward declaration — FFP_Engage is defined after this header is included */
static void FFP_Engage(WrappedDevice *self);

/* ---- Format Decoders ---- */

/* IEEE 754 binary16 (FLOAT16_2) → binary32 */
static float half_to_float(unsigned short h) {
    unsigned int sign = (unsigned int)(h >> 15) << 31;
    unsigned int exp  = (h >> 10) & 0x1F;
    unsigned int mant = (unsigned int)(h & 0x3FF);
    unsigned int f;
    if (exp == 0) {
        if (mant == 0) { f = sign; }
        else {
            exp = 113;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            f = sign | (exp << 23) | ((mant & 0x3FF) << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000 | (mant << 13);
    } else {
        f = sign | ((exp + 112) << 23) | (mant << 13);
    }
    { float r; memcpy(&r, &f, 4); return r; }
}

/* Decode a compressed normal to FLOAT3.
 * Handles SHORT4N, DEC3N, UDEC3, and direct FLOAT3/FLOAT4. */
static void decode_normal(float *out3, const unsigned char *src, int type) {
    switch (type) {
        case D3DDECLTYPE_SHORT4N: {
            signed short s0 = (signed short)((unsigned short)(src[0] | ((unsigned short)src[1] << 8)));
            signed short s1 = (signed short)((unsigned short)(src[2] | ((unsigned short)src[3] << 8)));
            signed short s2 = (signed short)((unsigned short)(src[4] | ((unsigned short)src[5] << 8)));
            out3[0] = s0 / 32767.0f;
            out3[1] = s1 / 32767.0f;
            out3[2] = s2 / 32767.0f;
            break;
        }
        case D3DDECLTYPE_DEC3N: {
            unsigned int p = src[0] | ((unsigned int)src[1]<<8) | ((unsigned int)src[2]<<16) | ((unsigned int)src[3]<<24);
            int x = (int)(p & 0x3FF);         if (x >= 512) x -= 1024;
            int y = (int)((p >> 10) & 0x3FF);  if (y >= 512) y -= 1024;
            int z = (int)((p >> 20) & 0x3FF);  if (z >= 512) z -= 1024;
            out3[0] = x / 511.0f;
            out3[1] = y / 511.0f;
            out3[2] = z / 511.0f;
            break;
        }
        case D3DDECLTYPE_UDEC3: {
            unsigned int p = src[0] | ((unsigned int)src[1]<<8) | ((unsigned int)src[2]<<16) | ((unsigned int)src[3]<<24);
            out3[0] = (float)(p & 0x3FF) / 1023.0f;
            out3[1] = (float)((p >> 10) & 0x3FF) / 1023.0f;
            out3[2] = (float)((p >> 20) & 0x3FF) / 1023.0f;
            break;
        }
        default: { /* FLOAT3, FLOAT4: direct copy of first 3 components */
            float *f = (float*)src;
            out3[0] = f[0]; out3[1] = f[1]; out3[2] = f[2];
            break;
        }
    }
}

/* ---- Vertex Expansion ---- */

/*
 * Expand one source vertex (src) into the fixed 48-byte skinned layout (dst):
 *   offset  0: FLOAT3 POSITION
 *   offset 12: FLOAT3 BLENDWEIGHT (3 components; unused slots padded with 0.0)
 *   offset 24: UBYTE4 BLENDINDICES (copied as-is)
 *   offset 28: FLOAT3 NORMAL      (decoded from any compressed source format)
 *   offset 40: FLOAT2 TEXCOORD[0] (decoded from FLOAT16_2, or direct copy)
 */
static void expand_skin_vertex(unsigned char *dst, const unsigned char *src, WrappedDevice *self) {
    float *dstF = (float*)dst;

    /* POSITION: FLOAT3 */
    { float *sp = (float*)(src + self->curDeclPosOff); dstF[0]=sp[0]; dstF[1]=sp[1]; dstF[2]=sp[2]; }

    /* BLENDWEIGHT: up to FLOAT3, pad remaining with 0 */
    {
        int bwT = self->curDeclBlendWeightType;
        if (bwT >= D3DDECLTYPE_FLOAT1 && bwT <= D3DDECLTYPE_FLOAT4) {
            float *sw = (float*)(src + self->curDeclBlendWeightOff);
            dstF[3] = sw[0];
            dstF[4] = (bwT >= D3DDECLTYPE_FLOAT2) ? sw[1] : 0.0f;
            dstF[5] = (bwT >= D3DDECLTYPE_FLOAT3) ? sw[2] : 0.0f;
        } else if (bwT == D3DDECLTYPE_UBYTE4N) {
            const unsigned char *sw = src + self->curDeclBlendWeightOff;
            dstF[3] = sw[0] / 255.0f;
            dstF[4] = sw[1] / 255.0f;
            dstF[5] = sw[2] / 255.0f;
        } else { dstF[3] = 0.0f; dstF[4] = 0.0f; dstF[5] = 0.0f; }
    }

    /* BLENDINDICES: UBYTE4 → 4 bytes at dst offset 24 */
    { const unsigned char *si = src + self->curDeclBlendIndicesOff;
      dst[24]=si[0]; dst[25]=si[1]; dst[26]=si[2]; dst[27]=si[3]; }

    /* NORMAL: decoded to FLOAT3 at dst offset 28 (dstF[7..9]) */
    if (self->curDeclNormalType >= 0)
        decode_normal(&dstF[7], src + self->curDeclNormalOff, self->curDeclNormalType);
    else
        { dstF[7]=0.0f; dstF[8]=0.0f; dstF[9]=1.0f; }

    /* TEXCOORD[0]: output FLOAT2 at dst offset 40 (dstF[10..11]) */
    { int uvT = self->curDeclTexcoordType;
      if (uvT == D3DDECLTYPE_FLOAT16_2) {
          unsigned short *sh = (unsigned short*)(src + self->curDeclTexcoordOff);
          dstF[10] = half_to_float(sh[0]); dstF[11] = half_to_float(sh[1]);
      } else if (uvT >= D3DDECLTYPE_FLOAT1 && uvT <= D3DDECLTYPE_FLOAT4) {
          float *su = (float*)(src + self->curDeclTexcoordOff);
          dstF[10] = su[0];
          dstF[11] = (uvT >= D3DDECLTYPE_FLOAT2) ? su[1] : 0.0f;
      } else { dstF[10]=0.0f; dstF[11]=0.0f; }
    }
}

/* ---- Vertex Buffer Caching ---- */

/*
 * Look up or build an expanded vertex buffer for source vertices [baseVtx, baseVtx+nv-1].
 * Results are cached by a hash of (source VB ptr, baseVtx, nv, stride, decl ptr).
 * Returns the expanded IDirect3DVertexBuffer9* or NULL on failure.
 */
static void* SkinVB_GetExpanded(WrappedDevice *self, unsigned int baseVtx, unsigned int nv) {
    typedef int (__stdcall *FN_CreateVB)(void*, unsigned int, unsigned long, unsigned long, unsigned int, void**, void*);
    typedef int (__stdcall *FN_Lock)   (void*, unsigned int, unsigned int, void**, unsigned int);
    typedef int (__stdcall *FN_Unlock) (void*);
    typedef unsigned long (__stdcall *FN_Release)(void*);
    unsigned int key, v, srcStride, lockOff, lockSize;
    int slot, hrC, hrL;
    void *newVB, **srcVt, **dstVt;
    unsigned char *srcData, *dstData;

    if (!self->streamVB[0]) return NULL;

    key  = (unsigned int)self->streamVB[0]
         ^ (baseVtx * 2654435761u)
         ^ (nv * 40503u)
         ^ (self->streamStride[0] * 6700417u)
         ^ ((unsigned int)self->lastDecl * 2246822519u);
    slot = (int)(key % SKIN_CACHE_SIZE);

    if (self->skinExpVB[slot] && self->skinExpKey[slot] == key && self->skinExpNv[slot] == nv)
        return self->skinExpVB[slot];

    if (self->skinExpVB[slot]) {
        ((FN_Release)(*(void***)(self->skinExpVB[slot]))[2])(self->skinExpVB[slot]);
        self->skinExpVB[slot] = NULL;
    }

    newVB = NULL;
    hrC = ((FN_CreateVB)RealVtbl(self)[SLOT_CreateVertexBuffer])(
        self->pReal, nv * SKIN_VTX_SIZE, 8 /*WRITEONLY*/, 0, 1 /*MANAGED*/, &newVB, NULL);
    if (hrC != 0 || !newVB) { log_hex("SkinVB CreateVB failed hr=", (unsigned int)hrC); return NULL; }

    srcStride = self->streamStride[0];
    lockOff   = self->streamOffset[0] + baseVtx * srcStride;
    lockSize  = nv * srcStride;
    srcVt     = *(void***)self->streamVB[0];
    srcData   = NULL;
    hrL = ((FN_Lock)srcVt[11])(self->streamVB[0], lockOff, lockSize, (void**)&srcData, 0x10 /*READONLY*/);
    if (hrL != 0 || !srcData) {
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        log_hex("SkinVB src lock failed hr=", (unsigned int)hrL); return NULL;
    }

    dstVt   = *(void***)newVB;
    dstData = NULL;
    hrL = ((FN_Lock)dstVt[11])(newVB, 0, nv * SKIN_VTX_SIZE, (void**)&dstData, 0);
    if (hrL != 0 || !dstData) {
        ((FN_Unlock)srcVt[12])(self->streamVB[0]);
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        return NULL;
    }

    for (v = 0; v < nv; v++)
        expand_skin_vertex(dstData + v * SKIN_VTX_SIZE, srcData + v * srcStride, self);

    ((FN_Unlock)dstVt[12])(newVB);
    ((FN_Unlock)srcVt[12])(self->streamVB[0]);

    self->skinExpVB[slot]  = newVB;
    self->skinExpKey[slot] = key;
    self->skinExpNv[slot]  = nv;
    return newVB;
}

/* Release all cached expanded vertex buffers */
static void SkinVB_ReleaseCache(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < SKIN_CACHE_SIZE; i++) {
        if (self->skinExpVB[i]) {
            ((FN_Release)(*(void***)(self->skinExpVB[i]))[2])(self->skinExpVB[i]);
            self->skinExpVB[i] = NULL;
        }
    }
}

/* ---- Bone Upload ---- */

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

    /*
     * After uploading bones, WORLDMATRIX(0) is clobbered by bone[0].
     * Flag world transform dirty so the next rigid draw's FFP_ApplyTransforms
     * re-applies SetTransform(WORLD) from vsConst.
     */
    self->worldDirty = 1;

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

/* ---- Skinning State Management ---- */

static void FFP_DisableSkinning(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    void **vt = RealVtbl(self);

    if (self->skinningSetup) {
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_INDEXEDVERTEXBLENDENABLE, 0);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
        self->skinningSetup = 0;
    }
}

/* ---- High-Level Skinning Entry Points ---- */

/*
 * Handle a skinned DrawIndexedPrimitive call.
 * Expands vertices to fixed FLOAT layout, uploads bones, draws with FFP.
 * Returns the HRESULT from the draw call.
 * Falls back to shader passthrough if expansion fails.
 */
static int Skin_DrawDIP(WrappedDevice *self, unsigned int pt, int bvi,
    unsigned int mi, unsigned int nv, unsigned int si, unsigned int pc)
{
    typedef int (__stdcall *FN_DIP)(void*, unsigned int, int, unsigned int, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetSS)(void*, unsigned int, void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetVD)(void*, void*);
    typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
    void **vt = RealVtbl(self);
    unsigned int baseVtx = (unsigned int)bvi + mi;
    void *expVB;
    int hr;

    expVB = (self->skinExpDecl) ? SkinVB_GetExpanded(self, baseVtx, nv) : NULL;

    if (expVB) {
        FFP_Engage(self);
        FFP_UploadBones(self);

        /* Albedo to stage 0, NULL remaining stages */
        {
            int as = self->albedoStage;
            void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
            int ts;
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
            for (ts = 1; ts < 8; ts++)
                ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
        }

        /* Bind the expansion VB and declaration */
        ((FN_SetSS)vt[SLOT_SetStreamSource])(self->pReal, 0, expVB, 0, SKIN_VTX_SIZE);
        ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, self->skinExpDecl);

        /* Vertices [mi..mi+nv-1] map to expansion slots [0..nv-1] via bvi=-mi */
        hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal,
            pt, -(int)mi, mi, nv, si, pc);

        /* Restore source VB, original declaration, and texture stages */
        ((FN_SetSS)vt[SLOT_SetStreamSource])(self->pReal, 0,
            self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
        ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
        {
            int ts;
            for (ts = 0; ts < 8; ts++)
                ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        }
    } else {
        /* Expansion unavailable (no skinExpDecl or VB lock failed) — shader passthrough */
        typedef int (__stdcall *FN_SetVS)(void*, void*);
        typedef int (__stdcall *FN_SetPS)(void*, void*);
        if (self->ffpActive) {
            ((FN_SetVS)vt[SLOT_SetVertexShader])(self->pReal, self->lastVS);
            ((FN_SetPS)vt[SLOT_SetPixelShader])(self->pReal, self->lastPS);
            self->ffpActive = 0;
        }
        hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
    }

    return hr;
}

/*
 * Create the shared expansion vertex declaration at device creation.
 * All skinned draws are expanded into a fixed 48-byte layout.
 */
static void Skin_InitDevice(WrappedDevice *w, void *pRealDevice) {
    typedef int (__stdcall *FN_CreateDecl)(void*, void*, void**);
    typedef struct { unsigned short Stream; unsigned short Offset;
                     unsigned char Type; unsigned char Method;
                     unsigned char Usage; unsigned char UsageIndex; } D3DVE;
    static const D3DVE s_skin_decl[] = {
        {0,  0, 2/*FLOAT3*/,0, 0/*POSITION*/,    0},
        {0, 12, 2/*FLOAT3*/,0, 1/*BLENDWEIGHT*/, 0},
        {0, 24, 5/*UBYTE4*/,0, 2/*BLENDINDICES*/,0},
        {0, 28, 2/*FLOAT3*/,0, 3/*NORMAL*/,      0},
        {0, 40, 1/*FLOAT2*/,0, 5/*TEXCOORD*/,    0},
        {0xFF,0,17,          0, 0,                0}  /* D3DDECL_END */
    };

    w->curDeclNumWeights = 0;
    w->boneStartReg = 0;
    w->numBones = 0;
    w->skinningSetup = 0;
    w->curDeclNormalOff = 0;   w->curDeclNormalType = -1;
    w->curDeclBlendWeightOff = 0;
    w->curDeclBlendWeightType = 0;
    w->curDeclBlendIndicesOff = 0;
    w->curDeclPosOff = 0;
    { int i; for (i = 0; i < SKIN_CACHE_SIZE; i++) { w->skinExpVB[i] = NULL; w->skinExpKey[i] = 0; w->skinExpNv[i] = 0; } }
    w->skinExpDecl = NULL;

    ((FN_CreateDecl)RealVtbl(w)[SLOT_CreateVertexDeclaration])(
        pRealDevice, (void*)s_skin_decl, &w->skinExpDecl);
    log_hex("  skinExpDecl: ", (unsigned int)w->skinExpDecl);
}

/* Free VB cache + skinExpDecl on device release */
static void Skin_ReleaseDevice(WrappedDevice *self) {
    SkinVB_ReleaseCache(self);
    if (self->skinExpDecl) {
        typedef unsigned long (__stdcall *FN_Release)(void*);
        ((FN_Release)(*(void***)self->skinExpDecl)[2])(self->skinExpDecl);
        self->skinExpDecl = NULL;
    }
}
