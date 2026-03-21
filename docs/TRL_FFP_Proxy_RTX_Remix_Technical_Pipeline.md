# Tomb Raider Legend FFP Proxy — RTX Remix Technical Pipeline

**How a d3d9.dll proxy DLL converts a shader-based D3D9 game into a path-traced scene for NVIDIA RTX Remix**

---

## Table of Contents

1. [Runtime Stack Overview](#1-runtime-stack-overview)
2. [The DLL Chain: Who Loads Who](#2-the-dll-chain-who-loads-who)
3. [The Fundamental Mismatch: What the Game Sends vs What Remix Needs](#3-the-fundamental-mismatch-what-the-game-sends-vs-what-remix-needs)
4. [The Proxy's 119-Method Vtable Wrapper](#4-the-proxys-119-method-vtable-wrapper)
5. [Matrix Recovery: The WVP Decomposition](#5-matrix-recovery-the-wvp-decomposition)
6. [Draw Call Routing: The Per-Draw Decision Tree](#6-draw-call-routing-the-per-draw-decision-tree)
7. [Texture Stage Cleanup](#7-texture-stage-cleanup)
8. [What Remix Sees After the Proxy](#8-what-remix-sees-after-the-proxy)
9. [Hash Stability: VP Inverse Caching and Matrix Quantization](#9-hash-stability-vp-inverse-caching-and-matrix-quantization)
10. [The dxwrapper SetTransform Conflict](#10-the-dxwrapper-settransform-conflict)
11. [Culling: Game-Side Patches and Remix Anti-Culling](#11-culling-game-side-patches-and-remix-anti-culling)
12. [The Full Frame Lifecycle](#12-the-full-frame-lifecycle)
13. [Diagnostic Logging](#13-diagnostic-logging)
14. [Deployment Layout](#14-deployment-layout)
15. [Remix Configuration Reference](#15-remix-configuration-reference)
16. [Known Issues and Decision Tree](#16-known-issues-and-decision-tree)

---

## 1. Runtime Stack Overview

Tomb Raider Legend (2006) is natively a Direct3D 9.0c game internally (SM 2.0/3.0 shaders), but it presents a D3D8 interface externally. The full runtime stack at game launch:

```
trl.exe (D3D8 API calls)
  → dxwrapper.dll        (D3D8 → D3D9 translation, elishacloud)
    → d3d9.dll           (FFP proxy — our code)
      → d3d9.dll.bak     (RTX Remix bridge/runtime, renamed)
        → system d3d9.dll (Windows DirectX runtime)
          → GPU driver
            → DXVK-Remix Vulkan renderer
              → RTXDI + ReSTIR GI + NRD + DLSS
```

Each layer in this stack has a specific role, and the proxy sits at the critical junction between the game's shader-based rendering and Remix's fixed-function scene reconstruction.

---

## 2. The DLL Chain: Who Loads Who

### The Load Sequence

When `trl.exe` starts and makes its first D3D8 call, the chain-loading sequence is:

1. **trl.exe** calls D3D8 `Direct3DCreate8()`
2. **dxwrapper.dll** intercepts, translates to D3D9, calls `Direct3DCreate9()`
3. **d3d9.dll** (the FFP proxy) exports `Direct3DCreate9` — this is the function that runs
4. The proxy reads `proxy.ini`: `[Remix] Enabled=1, DLLName=d3d9.dll.bak`
5. The proxy calls `LoadLibrary("d3d9.dll.bak")` — this loads RTX Remix
6. The proxy calls Remix's `Direct3DCreate9()`, gets back Remix's `IDirect3D9*`
7. The proxy wraps it in `WrappedD3D9` and returns that to dxwrapper
8. When dxwrapper later calls `CreateDevice`, the proxy's `W9_CreateDevice` intercepts it
9. Remix creates the real `IDirect3DDevice9*`
10. The proxy wraps it in `WrappedDevice` (the 119-method FFP conversion layer)
11. The wrapped device is returned to dxwrapper → game

### The Critical Implication

From this point forward, **every single D3D9 call the game makes passes through the proxy's vtable first**, then reaches Remix's device. Remix thinks it's talking to a normal D3D9 application. The proxy is invisible to it.

### Chain Loading Code (`d3d9_main.c`)

```c
// Read proxy.ini configuration
useRemix = GetPrivateProfileIntA("Remix", "Enabled", 0, iniBuf);

if (useRemix) {
    // Load Remix's d3d9.dll (renamed to d3d9.dll.bak)
    g_realD3D9 = LoadLibraryA("d3d9.dll.bak");
    g_realDirect3DCreate9 = GetProcAddress(g_realD3D9, "Direct3DCreate9");
}

// Call Remix's Direct3DCreate9, wrap the result
pReal = g_realDirect3DCreate9(SDKVersion);
return (void*)WrappedD3D9_Create(pReal);
```

---

## 3. The Fundamental Mismatch: What the Game Sends vs What Remix Needs

### What Remix's Scene Manager Listens For

RTX Remix's Scene Manager (`src/dxvk/rtx_render/` in the DXVK-Remix source) intercepts specific **fixed-function pipeline** D3D9 calls to reconstruct a 3D scene for path tracing:

| D3D9 FFP Call | What Remix Extracts |
|---|---|
| `SetTransform(D3DTS_WORLD, ...)` | Object position/rotation/scale in world space |
| `SetTransform(D3DTS_VIEW, ...)` | Camera position and orientation |
| `SetTransform(D3DTS_PROJECTION, ...)` | Camera FOV, near/far planes |
| `SetLight(index, D3DLIGHT9*)` | Light type, position, color, range |
| `SetMaterial(D3DMATERIAL9*)` | Surface reflectance properties |
| `SetTexture(stage, texture)` | Texture bindings for hash-based replacement |
| `DrawIndexedPrimitive()` | Geometry data (vertices, indices) for hashing |

### What TRL Actually Sends

TRL's render loop (through dxwrapper) sends shader-based calls:

```
SetVertexShader(pCompiledVS)              ← activates a vs_2_0 shader
SetVertexShaderConstantF(0, wvpData, 4)   ← uploads fused WVP to c0-c3
SetVertexShaderConstantF(4, worldData, 4) ← uploads World to c4-c7
SetPixelShader(pCompiledPS)               ← activates a ps_2_0 shader
SetTexture(0, albedo)
SetTexture(1, normalMap)
SetTexture(2, shadowMap)
DrawIndexedPrimitive(D3DPT_TRIANGLELIST, ...)
```

### The Gap

| What Remix Wants | What TRL Sends | Result Without Proxy |
|---|---|---|
| `SetTransform(D3DTS_WORLD, worldMatrix)` | Nothing — dxwrapper sends identity | No object positions |
| `SetTransform(D3DTS_VIEW, viewMatrix)` | Nothing — dxwrapper sends identity | No camera |
| `SetTransform(D3DTS_PROJECTION, projMatrix)` | Nothing — dxwrapper sends identity | No perspective |
| `SetVertexShader(NULL)` for FFP vertex processing | `SetVertexShader(pVS)` non-NULL | Remix can't interpret VS output |
| Texture on stage 0 only | Textures on stages 0–3 | Remix may hash wrong texture |

**Without the proxy**: Remix hooks successfully (Alt+X menu appears), but reports `"Trying to raytrace but not detecting a valid camera"` because every `SetTransform` it sees contains identity matrices.

---

## 4. The Proxy's 119-Method Vtable Wrapper

### Architecture

`IDirect3DDevice9` has 119 COM methods. The proxy wraps them all via a manually-built C vtable:

- **~15 methods are intercepted** with full C implementations (draw calls, shader management, texture tracking, transforms, etc.)
- **~104 methods relay directly** to the real device via zero-overhead `__declspec(naked)` ASM thunks

### Relay Thunk Pattern (Zero Overhead)

```c
#define RELAY_THUNK(name, slot) \
    static __declspec(naked) void __stdcall name(void) { \
        __asm { mov eax, [esp+4] }      /* eax = WrappedDevice* */ \
        __asm { mov ecx, [eax+4] }      /* ecx = pReal */ \
        __asm { mov [esp+4], ecx }      /* replace 'this' pointer */ \
        __asm { mov eax, [ecx] }        /* eax = real vtable */ \
        __asm { jmp dword ptr [eax + slot*4] } \
    }
```

This replaces the `this` pointer on the stack with the real device pointer and jumps directly to the real vtable entry — no extra stack frame, no function call overhead.

### Intercepted Methods

| Slot | Method | What the Proxy Does |
|---|---|---|
| 16 | `Reset` | Invalidates cached state, releases shader refs |
| 17 | `Present` | Resets per-frame counters, logs diagnostics, disengages FFP |
| 41/42 | `BeginScene`/`EndScene` | Tracks scene count, resets FFP setup flag |
| 44 | `SetTransform` | **Blocks dxwrapper's identity overrides** while FFP is active |
| 57 | `SetRenderState` | Forces `D3DCULL_NONE` and `D3DFILL_SOLID` globally |
| 65 | `SetTexture` | Tracks `curTexture[8]` for save/restore around FFP draws |
| 81 | `DrawPrimitive` | Suppresses line primitives, forces solid fill |
| 82 | `DrawIndexedPrimitive` | **Core FFP conversion** — routing, decomposition, draw |
| 83/84 | `DrawPrimitiveUP`/`DrawIndexedPrimitiveUP` | Suppresses line primitives |
| 87 | `SetVertexDeclaration` | Parses vertex elements, detects skinning/FLOAT3/NORMAL |
| 92 | `SetVertexShader` | Caches shader pointer, clears FFP active flag |
| 94 | `SetVertexShaderConstantF` | **Captures constants**, dirty tracking, bone detection |
| 100 | `SetStreamSource` | Tracks `streamStride[4]` and `streamVB[4]` |
| 107 | `SetPixelShader` | Caches shader pointer, swallows calls during FFP mode |
| 109 | `SetPixelShaderConstantF` | Captures PS constants for diagnostics |

---

## 5. Matrix Recovery: The WVP Decomposition

This is the hardest part of the port. TRL's shaders receive a fused WorldViewProjection matrix in VS constant registers c0–c3. Remix needs separated World, View, and Projection matrices.

### TRL's VS Constant Register Layout (From CTAB Analysis)

```
c0–c3   WorldViewProjection  (4 regs, 4x4 matrix — column-major in constants)
c4–c7   World                (4 regs, 4x4 matrix)
c8–c11  View                 (partial, lighting-only — often zero)
c12–c15 ViewProjection       (4 regs — second half of c8 count=8 upload, often zero)
c16     CameraPos            (1 reg)
c24     ModulateColor0       (1 reg)
c26     TextureScroll        (1 reg)
c39     Constants            (1 reg: {2.0, 0.5, 0.0, 1.0})
c48+    SkinMatrices         (48 regs: 16 bones × 3 regs per bone)
```

### The Decomposition Algorithm

The proxy reads View and Projection matrices directly from game memory (discovered via reverse engineering), not from VS constants:

```c
// Game memory addresses (fixed .data section, no pointer indirection)
#define TRL_VIEW_ADDR   0x010FC780  // float[16] row-major View matrix
#define TRL_PROJ_ADDR   0x01002530  // float[16] row-major Projection matrix
```

The decomposition in `FFP_ApplyTransforms`:

```
Step 1: Transpose c0-c3 from column-major (shader convention)
        to row-major (D3D9 SetTransform convention)
        → wvpRM[16]

Step 2: Read View and Projection from game memory every draw
        (must be fresh per-draw because multi-pass rendering
        changes V/P mid-scene for shadow maps, reflections)
        → gameView[16], gameProj[16]

Step 3: Compute VP = View × Projection
        → vp[16]

Step 4: Invert VP (cached when VP hasn't changed — see §9)
        → vpInv[16]

Step 5: Decompose World = WVP × inv(VP)
        → world[16]

Step 6: Quantize World to 1e-3 grid (eliminates FP jitter — see §9)
        → world[16] (snapped)

Step 7: Feed separated matrices to Remix via SetTransform
        SetTransform(D3DTS_WORLD, world)
        SetTransform(D3DTS_VIEW, gameView)
        SetTransform(D3DTS_PROJECTION, gameProj)
```

### Why Not Use c4–c7 (World) or c12–c15 (ViewProjection) Directly?

TRL uploads c8 with count=8, which covers both c8–c11 (View) and c12–c15 (ViewProjection). But these registers are **often zero** during gameplay — they're populated only for certain shader techniques. The WVP at c0–c3 is the only reliably non-zero matrix for every draw call.

The game memory addresses for View and Projection contain the authoritative camera state that the game's matrix computation code uses *before* fusing into WVP. Reading them directly bypasses the constant upload pipeline entirely.

---

## 6. Draw Call Routing: The Per-Draw Decision Tree

Every `DrawIndexedPrimitive` call goes through routing logic that decides whether to apply FFP conversion or pass through with original shaders.

### TRL-Specific Decision Tree

```
Is wvpValid? (has a non-zero WVP been captured at c0-c3?)
├─ NO → shader passthrough (pre-menu, loading screens, no camera yet)
└─ YES, and primitive type is TRIANGLELIST, and stream stride >= 12
    │
    ├─ Is curDeclPosIsFloat3? (POSITION element is D3DDECLTYPE_FLOAT3)
    │   │
    │   ├─ Is WVP ≈ Projection matrix? (within 0.05 tolerance per element)
    │   │   → Screen-space quad (post-process, bloom, tonemapping)
    │   │   → SKIP: return 0 (don't draw at all)
    │   │
    │   └─ WVP ≠ Projection
    │       → Character geometry (Lara, NPCs)
    │       → FFP_Engage → decompose W/V/P → draw with shaders still active
    │       → (Remix vertex capture intercepts post-VS positions)
    │
    ├─ Is curDeclIsSkinned? (has BLENDWEIGHT + BLENDINDICES)
    │   → Skinned mesh (not actually used by TRL — kept for safety)
    │   → Set V/P from game memory, World = identity → draw with shaders
    │
    └─ Else: rigid world mesh
        → FFP_Engage → decompose W/V/P → draw
        → Rebind albedo to stage 0, NULL stages 1-7
```

### Vertex Declaration Patterns

The proxy parses every `SetVertexDeclaration` call to classify geometry:

| Format | Elements | Route |
|---|---|---|
| Rigid world geometry | SHORT4 POSITION + NORMAL + TEXCOORD | FFP convert (decompose + draw) |
| Characters (Lara, NPCs) | FLOAT3 POSITION + NORMAL + TEXCOORD (no BLENDWEIGHT) | FFP Engage + shader draw |
| HUD / UI | POSITION (no NORMAL), or POSITIONT | Shader passthrough |
| Fullscreen quads | FLOAT3 POSITION where WVP ≈ Proj | Skip entirely (return 0) |
| Particles | POSITION + TEXCOORD (no NORMAL, non-indexed) | Shader passthrough via DrawPrimitive |

### Screen-Space Quad Detection

TRL's "Next Generation Content" mode uses post-processing passes (bloom, HDR tonemapping, color grading). These render as fullscreen quads that **must not** enter the path tracing pipeline. The proxy detects them by comparing the current WVP to the Projection matrix — if they match (within tolerance), the draw is a screen-space operation:

```c
mat4_transpose(wvpCheck, &self->vsConst[VS_REG_WVP_START * 4]);
float *gP = (float *)TRL_PROJ_ADDR;
int isScreenSpace = 1;
for (pi = 0; pi < 16; pi++) {
    float diff = wvpCheck[pi] - gP[pi];
    if (diff > 0.05f || diff < -0.05f) { isScreenSpace = 0; break; }
}
if (isScreenSpace) { self->quadSkips++; return 0; }
```

---

## 7. Texture Stage Cleanup

### The Problem

TRL binds multiple textures for its pixel shaders:
- Stage 0: Albedo/diffuse
- Stage 1: Normal map
- Stage 2: Shadow map
- Stages 3+: LUTs, detail maps, etc.

When the proxy engages FFP mode, D3D9's fixed-function texture stages become active. If stages 1–7 still have textures bound, FFP will try to blend them, and Remix will hash the wrong textures as materials.

### The Solution: Save, Rebind, Draw, Restore

```c
// Before FFP draw:
// 1. Copy albedo to stage 0 (in case AlbedoStage != 0)
SetTexture(0, self->curTexture[self->albedoStage]);
// 2. NULL all other stages
for (ts = 1; ts < 8; ts++)
    SetTexture(ts, NULL);

// 3. Draw the geometry (Remix sees only the albedo)
DrawIndexedPrimitive(...);

// 4. Restore original bindings for subsequent shader draws
for (ts = 0; ts < 8; ts++)
    SetTexture(ts, self->curTexture[ts]);
```

This ensures:
- Remix hashes only the albedo texture for PBR material replacement
- Normal maps, shadow maps, and LUTs don't pollute the material pipeline
- Shader passthrough draws (HUD, particles) still get all their texture bindings

---

## 8. What Remix Sees After the Proxy

After the proxy processes a typical world geometry draw, the D3D9 call stream arriving at Remix's DXVK-Remix renderer:

```
SetTransform(D3DTS_WORLD, [object-to-world matrix])       ← decomposed from WVP
SetTransform(D3DTS_VIEW, [camera view matrix])             ← read from game memory
SetTransform(D3DTS_PROJECTION, [perspective matrix])       ← read from game memory
SetTexture(0, albedoTexture)                               ← cleaned, albedo only
SetTexture(1, NULL)                                        ← stripped
SetTexture(2, NULL)                                        ← stripped
DrawIndexedPrimitive(D3DPT_TRIANGLELIST, ...)             ← unchanged geometry
```

### How Remix's Scene Manager Processes This

1. **`SetTransform(D3DTS_WORLD)`** → extracts the per-object transform → becomes the BLAS instance transform in the TLAS acceleration structure.

2. **`SetTransform(D3DTS_VIEW)`** → extracts camera position/orientation → Remix knows where to cast primary rays from. The "not detecting a valid camera" error goes away.

3. **`SetTransform(D3DTS_PROJECTION)`** → extracts FOV and clip planes → validates the camera. `rtx.fusedWorldViewMode=0` means W, V, P are all separate, which matches the proxy's output.

4. **`DrawIndexedPrimitive`** → Remix reads the vertex/index buffers and computes:
   - **Geometry Asset Hash** (`positions + indices + geometrydescriptor`) → produces `mesh_XXXXXXXXXXXXXXXX` identifiers for USD replacement in the Toolkit
   - **Geometry Generation Hash** (asset hash + texcoords + vertexlayout) → tracks instances across frames for temporal algorithms (denoising, motion vectors)

5. **`SetTexture(0, albedo)`** → Remix hashes the texture pixel data → identifies materials for PBR replacement via the Toolkit's material editor.

### From Remix to Path Tracing

Once the Scene Manager has reconstructed the frame:

```
Scene Reconstruction
  → BLAS (Bottom-Level Acceleration Structure) per unique geometry hash
    → TLAS (Top-Level Acceleration Structure) for the frame
      → RTXDI: importance-sampled direct illumination
        → ReSTIR GI: reservoir-based indirect illumination
          → NRD: AI denoiser (ReLAX or ReBLUR)
            → DLSS: AI upscaling
              → Final path-traced output
```

---

## 9. Hash Stability: VP Inverse Caching and Matrix Quantization

### The Problem

Remix identifies objects by hashing their geometry data. For asset replacement and temporal coherence (denoising, motion vectors), the hash must be **identical** for the same static object across frames.

The WVP decomposition `World = WVP × inv(VP)` introduces floating-point instability: the same static object produces slightly different World matrices when the camera moves, because FP multiplication isn't associative. This causes geometry hashes to flicker in Remix's debug view.

### VP Inverse Cache

The proxy caches the previous VP and its inverse:

```c
// Compare current VP to cached VP (within epsilon)
int same = 1;
for (ci = 0; ci < 16; ci++) {
    float diff = vp[ci] - self->prevVP[ci];
    if (diff > 1e-4f || diff < -1e-4f) { same = 0; break; }
}

if (same) {
    // Reuse cached inverse — eliminates FP jitter
    memcpy(vpInv, self->prevVpInv, 64);
} else {
    // VP changed (new render pass) — recompute
    mat4_inverse(vpInv, vp);
    memcpy(self->prevVP, vp, 64);
    memcpy(self->prevVpInv, vpInv, 64);
    self->prevVpInvValid = 1;
}
```

This handles the common case where VP is the same across all draws in a frame (main camera), while still supporting mid-frame VP changes (shadow passes, reflections).

### Matrix Quantization

After decomposition, the World matrix is snapped to a grid:

```c
static void mat4_quantize(float *m, float grid) {
    float inv_grid = 1.0f / grid;
    for (i = 0; i < 16; i++) {
        float v = m[i] * inv_grid;
        m[i] = (float)(int)(v + (v >= 0.0f ? 0.5f : -0.5f)) * grid;
    }
}

// Grid size 1e-3: larger than typical FP error (~1e-5)
// but small enough for spatial accuracy (0.001 world units ≈ <1mm)
mat4_quantize(world, 1e-3f);
```

Together, the VP cache and quantization ensure that static objects produce **identical** World matrices and therefore **stable geometry hashes** across frames, enabling reliable asset replacement in the Toolkit.

---

## 10. The dxwrapper SetTransform Conflict

### The Problem

dxwrapper's D3D8→D3D9 translation calls `SetTransform` with `View=identity`, `Proj=identity`, `World=WVP`. If these reach the real device, they overwrite the proxy's decomposed transforms between draws.

### The Solution: SetTransform Intercept

```c
static int __stdcall WD_SetTransform(WrappedDevice *self,
    unsigned int state, float *pMatrix) {
    typedef int (__stdcall *FN)(void*, unsigned int, float*);
    // Block ALL SetTransform calls while FFP is active
    if (self->ffpActive) return 0;
    // Otherwise relay to real device
    return ((FN)RealVtbl(self)[SLOT_SetTransform])(self->pReal, state, pMatrix);
}
```

While `ffpActive` is true (between `FFP_Engage` and `FFP_Disengage`), all external `SetTransform` calls are swallowed. Only the proxy's own `SetTransform` calls (made from `FFP_ApplyTransforms` through the real vtable directly) reach Remix.

---

## 11. Culling: Game-Side Patches and Remix Anti-Culling

### Why Culling Matters for Path Tracing

Path tracing needs geometry visible from all directions — for reflections, global illumination, and shadows from off-screen light sources. TRL's engine frustum-culls geometry on the CPU before submitting draw calls, meaning anything outside the camera frustum is never sent to the GPU.

### Two-Layer Solution

**Layer 1: Game-Side Binary Patches** (make the game submit more geometry)

| Patch | Address | Original | Patched | Effect |
|---|---|---|---|---|
| Frustum threshold | `0x00EFDD64` | default float | 100000.0f | Distance check passes for nearly everything |
| Cull-mode conditional | `0x0040EEA7` | 15-byte bitfield test | `mov ecx, 1; nop×10` | Always-render flag forced |
| View distance | `0x010FC910` | game-set value | 100000.0f (per-frame) | Spatial tree traverses more nodes |
| Far clip plane | `0x00EFFECC` | 12288.0f | 100000.0f | Extended render distance |

> **Note**: These patches are currently disabled in the proxy because the spatial tree traverses too many nodes with the extended distance, causing freezes on level load. The shader-passthrough + transform-override approach works without them. A more surgical culling patch is needed.

**Layer 2: Remix Anti-Culling** (retain geometry that was drawn at least once)

```ini
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2.0
rtx.antiCulling.object.farPlaneScale = 10.0
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.antiCulling.object.enableInfinityFarPlane = True
```

Remix anti-culling retains objects that *were* drawn but left the frustum. It **cannot** force the game to submit objects it already culled on the CPU. That's why game-side patches are the primary solution, with Remix anti-culling as a supplement.

---

## 12. The Full Frame Lifecycle

One complete frame from game call to path-traced output:

### Phase 1: Frame Setup

1. **Game calls `BeginScene`** → `WD_BeginScene` resets `ffpSetup`, increments `sceneCount`, forwards to Remix.

### Phase 2: State Uploads

2. **Game uploads shader constants** → `WD_SetVertexShaderConstantF` copies data into `vsConst[256×4]`, sets dirty flags (`wvpDirty`, `worldDirty`, `viewProjDirty`), marks `wvpValid` once c0-c3 are non-zero. Forwards to Remix (which ignores it).

3. **Game sets vertex declaration** → `WD_SetVertexDeclaration` parses `D3DVERTEXELEMENT9` array via COM vtable call, detects FLOAT3 positions, BLENDWEIGHT/BLENDINDICES (skinning), NORMAL presence, POSITIONT (screen-space). Forwards to Remix.

4. **Game sets textures and stream sources** → `WD_SetTexture` tracks `curTexture[8]`, `WD_SetStreamSource` tracks `streamStride[4]` and `streamVB[4]`. Both forward to Remix.

### Phase 3: Draw Calls

5. **Game calls `DrawIndexedPrimitive`** → routing logic decides:

   **For rigid world mesh** (has NORMAL, not skinned, not screen-space):
   ```
   FFP_Engage()
     → FFP_ApplyTransforms()
       → Read gameView from 0x010FC780
       → Read gameProj from 0x01002530
       → Compute VP = View × Proj
       → Check VP cache → reuse or recompute inv(VP)
       → World = WVP × inv(VP)
       → Quantize World to 1e-3 grid
       → SetTransform(D3DTS_WORLD, world)      → reaches Remix
       → SetTransform(D3DTS_VIEW, gameView)     → reaches Remix
       → SetTransform(D3DTS_PROJECTION, gameProj) → reaches Remix
     → FFP_SetupTextureStages()
       → SetTexture(0, albedo)
       → SetTexture(1..7, NULL)
     → FFP_SetupLighting() (once per frame)
       → SetRenderState(D3DRS_LIGHTING, FALSE)
       → SetMaterial(white)
   DrawIndexedPrimitive(...)                    → reaches Remix
   Restore textures 0..7 to original bindings
   ```

   **For character geometry** (FLOAT3 position, WVP ≠ Proj):
   ```
   FFP_Engage()
     → Same decomposition as above
   DrawIndexedPrimitive(...)  // shaders still active, Remix vertex capture grabs positions
   ```

   **For screen-space quad** (WVP ≈ Proj):
   ```
   return 0;  // skip entirely — don't confuse Remix's scene reconstruction
   ```

   **For HUD/pre-camera/passthrough**:
   ```
   FFP_Disengage()  // restore game's shaders
   DrawIndexedPrimitive(...)  // Remix sees it as shader-based, ignores for path tracing
   ```

### Phase 4: Remix Rendering

6. **Remix's DXVK-Remix renderer** receives the `SetTransform` + `DrawIndexedPrimitive` stream:
   - Builds BLAS (Bottom-Level Acceleration Structure) per unique geometry hash
   - Assembles TLAS (Top-Level Acceleration Structure) for the frame
   - Launches RTXDI for importance-sampled direct illumination
   - Launches ReSTIR GI for reservoir-based indirect illumination
   - Applies NRD denoising (ReLAX or ReBLUR)
   - Applies DLSS AI upscaling

### Phase 5: Presentation

7. **Game calls `Present`** → `WD_Present`:
   - Logs diagnostics if within delay window (VS registers written, vertex declarations, draw routing counters, matrix values)
   - Resets per-frame counters (`ffpDraws`, `skinnedDraws`, `quadSkips`, `shaderDraws`, etc.)
   - Disengages FFP mode
   - Clears VS constant write log
   - Forwards to Remix, which composites and presents the path-traced frame

---

## 13. Diagnostic Logging

The proxy writes `ffp_proxy.log` in the game directory with detailed per-draw diagnostics. Logging activates after a configurable delay (default 15 seconds) and captures a configurable number of frames (default 10).

### What Gets Logged

Per frame (`Present`):
- Frame number, draw call count, scene count
- VS registers written (bitmask of c0–c255)
- Unique textures per stage
- Draw routing counters: FFP draws, skinned draws, quad skips, FLOAT3 passes, shader draws
- Decomposition counters: successful decompositions vs fallbacks
- Game memory matrices (View and Projection)

Per draw call (`DrawIndexedPrimitive`, first 200 per frame):
- Route taken (FFP, SKINNED, QUAD_SKIP, SHADER)
- Vertex count, primitive count, stream stride
- Has texcoord, has normal, wvp valid, ffp active
- Current vertex shader and pixel shader pointers
- Texture pointers per stage
- Raw vertex bytes (first 10 draws)
- VS constant register blocks c0–c39 (first 5 draws)
- WVP and Projection matrices (first 20 draws)

Per vertex declaration (logged once per unique declaration pointer):
- Full element list with stream, offset, type, usage, usage index
- Skinning detection flag
- Human-readable type and usage names

---

## 14. Deployment Layout

### Game Directory Structure

```
A:\SteamLibrary\steamapps\common\Tomb Raider Legend\
  trl.exe                ← game executable
  dxwrapper.dll          ← D3D8→D3D9 translation (elishacloud)
  dxwrapper.ini          ← dxwrapper configuration
  d3d9.dll               ← FFP proxy (built from patches/trl_legend_ffp/proxy/)
  d3d9.dll.bak           ← RTX Remix runtime (renamed from Remix's d3d9.dll)
  proxy.ini              ← FFP proxy configuration
  rtx.conf               ← Remix runtime configuration
  .trex/                 ← Remix mod folder (USD replacements, captures)
  ffp_proxy.log          ← generated at runtime after delay
```

### proxy.ini Configuration

```ini
[Remix]
Enabled=1                    ; Chain-load Remix
DLLName=d3d9.dll.bak        ; Remix DLL filename

[FFP]
AlbedoStage=0                ; Which texture stage has the diffuse/albedo
DisableNormalMaps=0          ; Strip non-albedo stages during FFP draws
```

### Build and Deploy

```bash
# Build the proxy
cd patches/trl_legend_ffp/proxy
build.bat    # or _build_now.bat for hardcoded VS path

# Sync to game directory
powershell -ExecutionPolicy Bypass -File "patches/trl_legend_ffp/sync_runtime_to_game.ps1"

# Sync and launch
powershell -ExecutionPolicy Bypass -File "patches/trl_legend_ffp/sync_runtime_to_game.ps1" -Launch
```

> **Important**: Working directory for trl.exe MUST be the game directory — the game resolves `bigfile.*` archives relative to CWD.

---

## 15. Remix Configuration Reference

### rtx.conf — Key Settings for TRL

```ini
# Camera / transform
rtx.fusedWorldViewMode = 0              # 0 = separate W, V, P (matches proxy output)

# Anti-culling (supplements game-side patches)
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2.0
rtx.antiCulling.object.farPlaneScale = 10.0
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.antiCulling.object.enableInfinityFarPlane = True

# Anti-culling for lights
rtx.antiCulling.light.enable = True
rtx.antiCulling.light.numFramesToExtendLightLifetime = 100
rtx.antiCulling.light.fovScale = 2.0

# Hashing
rtx.calculateMeshBoundingBox = True
rtx.antiCulling.object.hashInstanceWithBoundingBoxHash = False

# Texture categorization (populate with actual hashes)
rtx.ignoreTextures =                    # post-process / fullscreen quad textures
rtx.uiTextures =                        # HUD element textures

# Vertex capture (keep shaders active, let Remix capture post-VS positions)
rtx.useVertexCapture = True             # if FFP path fails for some geometry
```

### How rtx.fusedWorldViewMode Works

| Mode | D3DTS_WORLD contains | D3DTS_VIEW contains | D3DTS_PROJECTION contains |
|---|---|---|---|
| 0 (our mode) | Object-to-world (W) | Camera view (V) | Perspective (P) |
| 1 | Identity | Fused W×V | Separate P |
| 2 | Fused W×V | Identity | Separate P |

The proxy provides mode 0 (fully separated), which gives Remix maximum information for scene reconstruction.

---

## 16. Known Issues and Decision Tree

### Symptom → Cause Table

| Symptom | Likely Cause | Fix |
|---|---|---|
| "not detecting a valid camera" | View/Projection are identity/zero | Check game memory addresses, verify wvpValid is set |
| Scene visible but camera doesn't track | View matrix is static | Verify gameView address updates with camera movement |
| Everything at origin / piled up | World matrix is identity for all objects | WVP decomposition failed — check inv(VP) |
| Geometry explodes / wobbles | fusedWorldViewMode mismatch | Ensure rtx.conf has fusedWorldViewMode=0 |
| Hash flickering in debug view | World contains camera-dependent data | VP cache or quantization not working |
| Floating white quads in scene | Fullscreen post-process leaked into path tracing | Quad detection threshold too loose, or add to rtx.ignoreTextures |
| Missing geometry after camera turn | Game-side frustum culling | Apply culling patches or increase anti-culling settings |
| Grid artifacts at mesh seams | NRD denoiser + normal discontinuities | USD mesh replacements with shared vertex normals |
| Characters invisible | FLOAT3 detection routing to wrong path | Check curDeclPosIsFloat3 in log |

### What To Do Next: Decision Tree

```
Is the proxy loading and chain-loading Remix?
├─ NO → Fix deployment: dxwrapper.dll, d3d9.dll, d3d9.dll.bak, proxy.ini
└─ YES
    Is Remix reporting "valid camera"?
    ├─ NO → Camera recovery:
    │   ├─ Verify game memory addresses (View at 0x010FC780, Proj at 0x01002530)
    │   ├─ Check ffp_proxy.log for matrix values
    │   ├─ Verify wvpValid=1 in DIP log entries
    │   └─ Check fusedWorldViewMode=0 in rtx.conf
    └─ YES
        Is geometry visible in Remix debug views?
        ├─ NO → Draw routing: check ffp_proxy.log for FFP_Engage count,
        │       check if NORMAL filter rejects world geometry,
        │       verify stream stride >= 12
        └─ YES
            Is path tracing correct?
            ├─ Missing geometry → Culling: game-side patches + Remix anti-culling
            ├─ Grid artifacts → Normal discontinuities: USD mesh replacements
            ├─ Floating white quads → Fullscreen post-process: add to rtx.ignoreTextures
            ├─ Hash flickering → VP cache miss or quantization grid too fine
            └─ Looks correct → Ship it
```

---

## Appendix: Key Source Files

| File | Role |
|---|---|
| `patches/trl_legend_ffp/proxy/d3d9_device.c` | Core FFP proxy — 119-method vtable, draw routing, WVP decomposition |
| `patches/trl_legend_ffp/proxy/d3d9_main.c` | DLL entry, logging, Remix chain-loading, INI parsing |
| `patches/trl_legend_ffp/proxy/d3d9_wrapper.c` | `IDirect3D9` wrapper — intercepts `CreateDevice` |
| `patches/trl_legend_ffp/proxy/proxy.ini` | Runtime config: chain-load toggle, albedo stage |
| `patches/trl_legend_ffp/proxy/build.bat` | MSVC x86 no-CRT build script |
| `.claude/skills/trl-rtx-remix/SKILL.md` | Full TRL-specific skill documentation |
| `.claude/skills/dx9-ffp-port/SKILL.md` | General FFP proxy porting workflow |

### Key Game Memory Addresses (From Reverse Engineering)

| Address | Type | Description |
|---|---|---|
| `0x010FC780` | float[16] | Row-major View matrix |
| `0x01002530` | float[16] | Row-major Projection matrix |
| `0x010FC910` | float | Max view distance for spatial tree |
| `0x00EFDD64` | float | Frustum cull distance threshold |
| `0x0040EEA7` | code (15B) | Cull-mode conditional (render/skip decision) |
| `0x00EFFECC` | float | Far clipping plane |
| `0x00ECBA40` | function | SetVertexShaderConstantF helper (through dxwrapper) |
| `0x0060C7D0` | function | Render caller A — gameplay camera pass |
| `0x0060EBF0` | function | Render caller B — auxiliary pass |
| `0x00610850` | function | Render caller C — auxiliary pass |

### VS Constant Register Map (TRL-Specific)

| Register Range | Name | Usage |
|---|---|---|
| c0–c3 | WorldViewProjection | Combined WVP — primary decomposition source |
| c4–c7 | World | Object-to-world (4x3 packed, sometimes 4x4) |
| c8–c11 | View | Partial view matrix (lighting only, often zero) |
| c12–c15 | ViewProjection | View×Proj (often zero during gameplay) |
| c16 | CameraPos | Camera world position (1 reg) |
| c24 | ModulateColor0 | Color tint parameter |
| c26 | TextureScroll | UV animation offset |
| c39 | Constants | Utility vector {2.0, 0.5, 0.0, 1.0} |
| c48–c95 | SkinMatrices | Bone palette (16 bones × 3 regs, 4x3 packed) |
