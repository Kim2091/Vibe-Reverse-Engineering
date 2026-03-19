# Tomb Raider Legend x RTX Remix — Deep Analysis Report

*Generated 2026-03-19 from comprehensive analysis of all logs, configs, test artifacts, and proxy source code.*

---

## Executive Summary

Tomb Raider Legend (2006) has been partially integrated with NVIDIA RTX Remix for path-traced rendering via a custom D3D9 proxy DLL. After 13 failed test iterations (March 14-15) and a breakthrough session on March 17, the project reached a state where:

- **Remix chain-loading works** — proxy → Remix bridge → DXVK-Remix path tracer initializes cleanly every session
- **FFP draw routing works** — thousands of world geometry draws per frame are converted and submitted with separate World/View/Projection transforms
- **Camera detection works** — Remix detects a valid camera starting ~12 seconds into gameplay and tracks it continuously
- **Path tracing engages** — Remix ray query and trace ray modes activate and process geometry
- **68 camera cut events** logged across a 6-minute session — indicating camera tracking is functional but unstable
- **No confirmed "Yes" test result** — the current build has not been validated with a saved Yes/No result per the test naming convention

**Bottom line**: The rendering pipeline is connected end-to-end. The proxy successfully decomposes the game's fused WVP matrix into separate World/View/Projection for Remix. The remaining issues are configuration conflicts and stability problems, not fundamental architecture failures.

---

## 1. Runtime Architecture

### DLL Chain (Deployed)

```
trl.exe (D3D8 surface, internally D3D9 SM 2.0/3.0)
  └─ dxwrapper.dll (elishacloud D3D8→D3D9, D3d8to9=1)
       └─ d3d9.dll (FFP proxy, 19,456 bytes, shader-passthrough + transform override)
            └─ d3d9.dll.bak (Remix bridge client, remix-main+5a70985a)
                 └─ .trex/d3d9.dll (DXVK-Remix path tracer)
                      └─ .trex/NvRemixBridge64.exe (64-bit Remix server)
```

### Hardware
- CPU: Intel Core i9-14900K
- GPU: NVIDIA GeForce RTX 5090 (32 GB VRAM, driver 595.71.0)
- RAM: 63.72 GB
- Display: 3840x2160 @ 240 Hz
- OS: Windows 11 Home Build 26200

### Key Fact
TRL presents a **D3D8 API surface** but is internally D3D9 SM 2.0/3.0. The dxwrapper translation layer is non-optional. The proxy sits between dxwrapper's D3D9 output and Remix's D3D9 input.

---

## 2. What Works

### 2.1 Proxy Chain-Loading (Confirmed Stable)

Every session, the proxy correctly:
1. Loads and intercepts `Direct3DCreate9`
2. Chain-loads Remix bridge from `d3d9.dll.bak`
3. Wraps the Remix-created `IDirect3DDevice9`
4. Completes the Remix 32-bit↔64-bit bridge handshake (~3 seconds)

**Evidence** (ffp_proxy.log):
```
Remix enabled, loading: ...\d3d9.dll.bak
Direct3DCreate9 called, SDK version: 0x00000020
Real IDirect3D9: 0x019CEB50
Real device: 0x020B0048
WrappedDevice created with FFP conversion
```

**Evidence** (bridge32.log): `"Ack received! Handshake completed!"`
**Evidence** (bridge64.log): `"Server side D3D9 Device created successfully!"`

### 2.2 Matrix Decomposition (Working)

The proxy performs full W/V/P decomposition from two sources:

**Source 1 — Game memory (stable, authoritative):**
- View matrix: `0x010FC780` (row-major, camera-dependent, changes with camera movement)
- Projection matrix: `0x01002530` (row-major, stable across all sessions)

**Source 2 — VS constant c0-c3 (per-draw):**
- Contains the fused WorldViewProjection matrix
- Decomposed using: `World = transpose(WVP) * inverse(VP)`

**Projection matrix (stable across ALL sessions):**
```
row0: [ 2.00,   0.00,  0.00,  0.00 ]
row1: [ 0.00,  -2.28,  0.00,  0.00 ]   ← Y-flipped
row2: [ 0.00,   0.00,  1.00,  1.00 ]   ← W-passthrough (infinite far)
row3: [ 0.00,   0.00, -16.00, 0.00 ]   ← near=16 world units
```

**Decomposed World (per-object, example):**
```
row0: [ 1.00,  0.00,  0.00,  0.00 ]     ← identity rotation
row1: [ 0.00,  1.00,  0.00,  0.00 ]
row2: [ 0.00,  0.00,  1.00,  0.00 ]
row3: [ -31526.62, -2365.89, 58097.12, 1.00 ]  ← world-space position
```

**Decomposed View (camera-dependent, example):**
```
row0: [ -0.61,  0.13, -0.56,  0.00 ]
row1: [  0.43,  0.19, -0.80,  0.00 ]
row2: [  0.00, -1.14, -0.20,  0.00 ]
row3: [ -18308.28, 66942.23, -8261.97, 1.00 ]
```

This decomposition is physically plausible: identity-rotation World with large translation for static level geometry, camera-dependent View matrix, and stable Projection.

### 2.3 FFP Draw Routing (Active, High Volume)

The proxy routes world geometry draws through FFP conversion at high volume:

```
DIP #352027  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=10900   primCount=10
DIP #352028  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=10900   primCount=14
DIP #352032  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=47
DIP #352033  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=78
DIP #352034  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=37
DIP #352035  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=75
DIP #352036  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=39
DIP #352037  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=97
DIP #352038  route=FFP  ffpActive=1  wvpValid=1  stride0=20  numVerts=25574  primCount=83
```

Key observations:
- Vertex counts of 10,900-25,574 are real world geometry (not trivial quads)
- `wvpValid=1` and `ffpActive=1` on all draws — the FFP path is engaged
- `ps=0x00000000` — pixel shaders are NULLed for FFP draws
- Stride=20 corresponds to SHORT4 position + D3DCOLOR + SHORT2x2 texcoords
- Multiple different WVP matrices per frame — per-object transforms are working

### 2.4 Camera Detection by Remix (Breakthrough)

**This is the single most important diagnostic signal.** The Remix log shows:

**Phase 1 — No camera (startup/menu, 03:33:42–03:33:58):**
```
[03:33:45.711] [RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera.
```

**Phase 2 — Camera found (in-game, starting frame 1788):**
```
[03:33:58.548] Camera cut detected on frame 1788
[03:33:59.528] Camera cut detected on frame 1845
[03:34:13.326] Camera cut detected on frame 2550
```

**Phase 3 — Camera cut storm (03:36:18, frames 5871-5880):**
```
[03:36:18.430] Camera cut detected on frame 5871
[03:36:18.475] Camera cut detected on frame 5872
[03:36:18.509] Camera cut detected on frame 5873
[03:36:18.546] Camera cut detected on frame 5874
[03:36:18.582] Camera cut detected on frame 5875
[03:36:18.619] Camera cut detected on frame 5876
[03:36:18.657] Camera cut detected on frame 5877
[03:36:18.695] Camera cut detected on frame 5878
[03:36:18.732] Camera cut detected on frame 5879
[03:36:18.775] Camera cut detected on frame 5880
```
10 camera cuts in 345ms. This is NOT normal gameplay camera behavior.

**Total: 68 camera cut events across a 6-minute session (03:33:42 to 03:39:42).**

### 2.5 Texture Categorization (Mature, 150+ Hashes)

The rtx.conf contains well-developed texture classification built over multiple sessions:

| Category | Count | Purpose |
|----------|-------|---------|
| `ignoreTextures` | 67 | Post-process passes, fullscreen quads, bloom, HDR effects |
| `particleTextures` | 29 | Dust, rain, sparks, fire particles |
| `uiTextures` | 22 | HUD, menus, overlays |
| `smoothNormalsTextures` | 17 | Surfaces needing interpolated normals |
| `skyBoxTextures` | 7 | Sky dome textures |
| `decalTextures` | 4 | Surface detail decals |
| `worldSpaceUiTextures` | 2 | In-world UI elements |
| `animatedWaterTextures` | 1 | Water surface |
| `hideInstanceTextures` | 1 | Hidden/culled geometry |
| `raytracedRenderTargetTextures` | 1 | Raytraced RT |
| `worldSpaceUiBackgroundTextures` | 1 | World-space UI background |

This represents significant manual tagging work and is relatively complete for the tested level.

### 2.6 Anti-Culling Configuration (Active)

```ini
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2
rtx.antiCulling.object.farPlaneScale = 10
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.antiCulling.object.enableInfinityFarPlane = True
```

Anti-culling retains geometry from previous frames for ray tracing. Settings look aggressive enough for the TRL scene scale.

### 2.7 DX9 Frame Capture Infrastructure (Working)

The DX9 tracer successfully captured 2 complete frames:
- **148,696 D3D9 API calls** in a single JSONL file
- Shader bytecode + CTAB disassembly captured
- Full backtraces to trl.exe addresses
- Vertex declarations with element layouts
- Available for further analysis with `--shader-map`, `--const-provenance`, etc.

---

## 3. What Doesn't Work / Known Issues

### 3.1 No Confirmed Path-Traced Geometry Visible (All 13 Tests = "No")

Every test snapshot saved in `Reverse/tests/` across March 14-15 resulted in "No":

| # | Test | Date | Visual Result | Root Cause |
|---|------|------|---------------|------------|
| 1 | EarlyTest | 3/14 | Light blue void | No FFP conversion active |
| 2 | LightBlue2 | 3/14 22:19 | Light blue void | Same |
| 3 | FlashingLights | 3/14 22:26 | Strobing/flashing | Degenerate matrices |
| 4 | LightBlue | 3/15 02:01 | Light blue void | Passthrough only |
| 5 | FixedFunction | 3/15 03:45 | FFP active but no geometry | `viewProjValid=0` |
| 6 | FlashingLights2 | 3/15 03:48 | Strobing | Small degenerate values in View |
| 7 | Broken | 3/15 03:52 | Routing failure | FFP eligibility bug |
| 8 | Broken2 | 3/15 04:02 | Routing failure | Same |
| 9 | TriangleSlices-AgentRestore | 3/15 15:17 | Partial/corrupted geometry | Stride/stream handling wrong |
| 10 | LightBlue3-AgentPassthrough | 3/15 15:22 | Light blue void | Pure passthrough mode |
| 11 | FixedFunction2-AgentWorldview | 3/15 15:29 | No path tracing | `fusedWorldViewMode=1` + `zUp=True` |
| 12 | TriangleSlices2 | 3/15 15:40 | Partial/corrupted | Transform corruption |
| 13 | (Current 3/17+ build) | 3/17+ | **TBD — NOT YET SAVED** | Camera detected, FFP active |

The current build (post-3/17) has NOT been formally tested and saved. The proxy log and Remix log both show positive signals but visual confirmation is missing.

### 3.2 Camera Instability — Cut Storms

The 68 camera cut events cluster into **bursts**:

| Time | Frames | Cuts | Rate | Likely Cause |
|------|--------|------|------|-------------|
| 03:33:58-03:34:00 | 1788-1845 | 2 | Normal | Initial camera detection |
| 03:34:13 | 2550-2551 | 2 | ~1/s | Scene transition? |
| 03:34:51 | 4829-4834 | 6 | ~30ms apart | Matrix instability burst |
| 03:36:00 | 5000-5001 | 2 | ~1/s | Re-detection after pause |
| 03:36:18 | 5871-5880 | **10** | **~35ms apart** | **Severe instability** |
| 03:38:10-38:11 | 11531-11567 | **10** | ~35ms apart | Same pattern |
| 03:38:20-38:21 | 11740-11746 | **7** | ~50ms apart | Same pattern |

**Analysis**: The cut storms at 35ms intervals correspond to every-frame camera cuts. This means Remix sees the camera change significantly between consecutive frames. Two possible causes:

1. **View matrix bleeding into World matrix** — If the WVP decomposition has a precision issue, the World matrix would change every frame the camera moves, causing Remix to see a "new" camera position.

2. **VP inverse instability** — The proxy caches `VP_inverse` and reuses it when the camera hasn't moved (epsilon 1e-4). If the epsilon is too loose or tight, the inverse may flip between cached and recalculated values, causing alternating matrix decomposition results.

### 3.3 Remix Warning: "Pushing more unique buffers than supported"

```
[03:34:55.844] [RTX-Compatibility-Info] This application is pushing more unique buffers than
is currently supported - some objects may not raytrace.
```

This means TRL is creating more vertex/index buffers per frame than Remix can track. With 3,187 draws per frame and ~25K vertices per large draw, this is plausible. Consequence: some geometry silently drops out of the ray-traced scene.

### 3.4 Remix Warning: "Skipped drawcall, colour write disabled"

```
[03:34:44.359] [RTX-Compatibility-Info] Skipped drawcall, colour write disabled.
```

At least one draw call had color write disabled (shadow pass or depth pre-pass). Remix correctly skips these, but if legitimate geometry draws have color write disabled due to the game's render state management, they would be lost.

### 3.5 Configuration Conflicts (Active)

Three settings in the current configuration are problematic:

#### `d3d9.apitraceMode = True` (ACTIVE despite being commented out in dxvk.conf)

The Remix log at line 17 shows: `d3d9.apitraceMode = True`

This is set **somewhere other than dxvk.conf** (where it's commented out). It may be coming from Remix's internal app profile for trl.exe or from a previous `user.conf` save. API trace mode changes buffer update semantics, making all host-visible buffers cached and coherent. This was needed for the DX9 tracer DLL but should NOT be active during normal proxy operation.

**Impact**: May alter how vertex buffer data reaches Remix's geometry processing, potentially causing the "unique buffers" overflow warning.

#### `rtx.useVertexCapture = True`

Vertex capture mode intercepts post-vertex-shader geometry from the GPU. With FFP conversion active, this creates a dual-path conflict:
- FFP path: Remix reads SetTransform W/V/P to reverse-map geometry
- Vertex capture path: Remix reads raw post-VS positions directly

Both paths running simultaneously may cause duplicate geometry instances, hash collisions, or geometry flickering. The handoff document notes this should be `False` when FFP is working.

#### `rtx.skipDrawCallsPostRTXInjection = True`

This tells Remix to skip draw calls after its injection point. If the injection point doesn't align with the proxy's FFP conversion point, Remix may skip the converted geometry entirely. This is flagged as **Suspect #1** in the workspace analysis.

### 3.6 SHORT4 Vertex Positions Through FFP

The proxy logs show vertex declarations with `D3DDECLTYPE_SHORT4` at POSITION[0]:

```
[s0 +0] POSITION[0] SHORT4       ← compressed signed short positions
[s0 +8] COLOR[0] D3DCOLOR
[s0 +12] TEXCOORD[0] SHORT2
[s0 +16] TEXCOORD[1] SHORT2
stride=20
```

D3D9 FFP does **not** support SHORT4 position elements. The game's vertex shader handles the SHORT4→clip-space transformation using a scale+offset MAD instruction. When the proxy NULLs the pixel shader and routes through FFP, the SHORT4 position data is passed to the fixed-function transform pipeline, which interprets the raw short values as floating-point positions — producing completely wrong geometry.

However, the proxy log also shows `route=FFP` for these draws with seemingly correct WVP values. The "shader-passthrough" approach documented in the handoff keeps the vertex shader active (does NOT null it), so the VS would still handle SHORT4→clip-space correctly. The confusion: the log shows `ps=0x00000000`, meaning pixel shaders ARE nulled, but vertex shaders may still be active.

**Critical ambiguity**: Is the current proxy nulling vertex shaders, pixel shaders, or both? The log shows `vs=0x019F1290` (a real VS handle), suggesting vertex shaders are kept active. If so, SHORT4 handling should be correct.

### 3.7 Projection Matrix Y-Flip

The projection's `[1][1] = -2.28` (negative Y) is unusual for D3D9. Standard D3D9 left-handed projection has positive Y. The negative Y means clip-space Y is flipped, which Remix's camera validation may interpret as an inverted/invalid camera.

This could explain:
- Camera not detected until 12 seconds in (menu uses different projection path)
- Camera cut storms (Remix oscillating between "valid" and "not valid" interpretations)

### 3.8 Game-Side Frustum Culling Not Patched

`FrustumPatch=0` in proxy.ini. The game's CPU-side frustum culling is active, meaning geometry outside the game camera's view frustum is never submitted to the D3D9 device. Remix anti-culling can retain previously-seen geometry, but cannot force the game to submit geometry it never sent.

**Impact on path tracing**: Reflections, indirect lighting, and shadows from off-screen geometry will have missing data. This is a quality issue, not a functionality blocker — path tracing should still work for on-screen geometry.

---

## 4. VS Constant Register Layout (Ground Truth)

From the embedded CTAB in captured shader bytecode and live trace analysis:

| Register(s) | CTAB Name | Size | Content | Per-Draw? |
|-------------|-----------|------|---------|-----------|
| c0-c3 | `WorldViewProject` | 4 regs | **Only transform matrix** — fused WVP | Yes |
| c4 | `fogConsts` | 1 reg | Fog parameters | Per-frame |
| c6 | `textureScroll` | 1 reg | UV animation offset | Per-draw |
| c8-c15 | `bendConstants` + other | 8 regs | Vegetation bending (0.01 range values), **NOT camera matrices** | Per-frame |
| c16+ | `lightInfo` | variable | Per-vertex lighting data | Per-draw |
| c24 | `ModulateColor0` | 1 reg | Color tint | Per-draw |
| c28 | scalar | 1 reg | World-space offset | Per-draw |
| c39 | `Constants` | 1 reg | Utility: {2.0, 0.5, 0.0, 1.0} | Once |
| c40 | `envMatrix` | 2 regs | Environment mapping | Per-draw |
| c48-c95 | `SkinMatrices` | 48 regs | Bone palette (16 bones x 3, 4x3 packed) | Per-draw (skinned only) |

**Critical finding**: c4-c7 and c8-c15 are **NOT View/Projection matrices**. Previous proxy iterations that assumed c8=View, c12=ViewProjection were operating on fog/bend/lighting data, not camera matrices. This was the root cause of early test failures (FlashingLights, Broken states).

---

## 5. Draw Call Statistics (Per Frame)

From the DX9 frame trace (148,696 calls, 2 frames):

| Metric | Value |
|--------|-------|
| Total draws per frame | ~3,187 (all DrawIndexedPrimitive) |
| Fullscreen quads | ~1,380 (43% of draws) |
| Opaque rigid draws | ~548 |
| Alpha-tested draws | ~2,636 |
| Alpha-blended draws | ~1,778 |
| Unique vertex shaders | 6 |
| Unique pixel shaders | ~15-20 |

The high percentage of fullscreen quads (43%) is notable — these are screen-space post-processing passes (bloom, color grading, HDR tone mapping). The proxy must correctly identify and skip these to avoid generating floating polygon artifacts in the ray-traced scene.

---

## 6. Vertex Format Analysis

Two vertex declaration types observed:

### World Geometry (majority of draws)
```
[s0 +0]  POSITION[0]  SHORT4      ← compressed signed short positions
[s0 +8]  COLOR[0]     D3DCOLOR    ← per-vertex color/lighting
[s0 +12] TEXCOORD[0]  SHORT2      ← primary UV
[s0 +16] TEXCOORD[1]  SHORT2      ← secondary UV (lightmap?)
stride = 20 bytes
```

### Screen-Space / Special Passes
```
[s0 +0]  POSITION[0]  FLOAT3      ← screen-space or world-space float positions
[s0 +12] COLOR[0]     D3DCOLOR
[s0 +16] TEXCOORD[0]  FLOAT2
stride = 24 bytes
```

The proxy detects FLOAT3 position declarations as screen-space candidates and skips them. This is the correct heuristic — FLOAT3 positions in TRL correspond to post-processing quads, UI elements, and particle overlays.

---

## 7. Proxy Approach Evolution

### Failed Approaches (March 14-15)

| Approach | What It Did | Why It Failed |
|----------|-------------|---------------|
| **Naive matrix mapping** (c0=P, c8=V, c12=W) | Assumed standard register layout | c8-c15 are fog/bend data, not matrices |
| **Identity World + always apply** | Set World=identity for all draws | Geometry invisible — no per-object transform |
| **Fused WVP as World** | Set WVP into World slot, identity View/Proj | `fusedWorldViewMode` mismatch with Remix |
| **FFP + DisableNormalMaps=1** | Aggressive FFP with normal map stripping | Broke texture stage state |
| **FFP + FrustumPatch=1** | FFP with frustum culling override | Broken rendering |
| **FFP + ForceFfpSkinned=1** | Force all draws through FFP | Broke skinned geometry |

**Key lesson**: Every `proxy.ini` advanced feature that was turned ON caused rendering failures. The working state has ALL features OFF.

### Working Approach (March 17 — Current)

**Shader-Passthrough + Transform Override:**

1. **Keep vertex shaders active** — the game's VS handles SHORT4→clip-space correctly
2. **NULL pixel shaders** — remove game's pixel shading for Remix to replace
3. **Read game memory** for true View (`0x010FC780`) and Projection (`0x01002530`)
4. **Compute VP = View * Projection** (once per BeginScene)
5. **Compute VP_inverse** (once per BeginScene, cached with epsilon comparison)
6. **Per draw: World = transpose(WVP_c0) * VP_inverse**
7. **Call SetTransform** with decomposed World, View, Projection before each draw
8. **Block dxwrapper's SetTransform** during active draws
9. **Skip FLOAT3 draws** (screen-space quads)
10. **Force D3DCULL_NONE** globally
11. **In-memory patches**: frustum threshold = 1e30, cull mode forced

---

## 8. Configuration State Audit

### rtx.conf (36 active settings)

| Setting | Value | Assessment |
|---------|-------|------------|
| `rtx.fusedWorldViewMode` | 0 | CORRECT — proxy provides separate W/V/P |
| `rtx.useVertexCapture` | True | **WRONG** — conflicts with FFP path, should be False |
| `rtx.enableRaytracing` | True | Correct |
| `rtx.skipDrawCallsPostRTXInjection` | True | **SUSPECT** — may skip FFP geometry |
| `rtx.zUp` | False | Correct for D3D9 convention |
| `rtx.orthographicIsUI` | True | Correct |
| `rtx.antiCulling.object.enable` | True | Correct |
| `rtx.fallbackLightMode` | 2 | OK (distant + area) |
| `rtx.fallbackLightRadiance` | 10, 10, 10 | OK |
| `rtx.hashCollisionDetection.enable` | True | Correct for debugging |
| `rtx.allowCubemaps` | False | OK |
| `rtx.skyAutoDetect` | 0 | Disabled — using manual sky texture hashes |
| `rtx.calculateMeshBoundingBox` | True | Correct |

### proxy.ini

| Setting | Value | Assessment |
|---------|-------|------------|
| `Enabled` | 1 | Remix chain-load active |
| `DLLName` | d3d9.dll.bak | Correct |
| `AlbedoStage` | 0 | Correct for TRL |
| `DisableNormalMaps` | 0 | Correct — was 1 in failed builds |
| `ForceFfpSkinned` | 0 | Correct — skinning disabled |
| `ForceFfpNoTexcoord` | 0 | Correct |
| `FrustumPatch` | 0 | OK for now — quality issue, not blocker |
| `FrustumScaleMicros` | 100 | N/A when FrustumPatch=0 |

### dxvk.conf

All settings commented out (defaults). `d3d9.apitraceMode` is commented out but Remix shows it as True — set from Remix's internal app profile or a stale user.conf entry.

### dxwrapper.ini

- `D3d8to9=1` — CRITICAL, non-optional
- All other extensions disabled

---

## 9. Remix Diagnostic Messages (Timeline)

| Time | Frame | Message | Significance |
|------|-------|---------|-------------|
| 03:33:42 | 0 | Config loaded, NRC Ultra preset | Normal startup |
| 03:33:45 | ~100 | "Not detecting valid camera" | Expected — main menu, no transforms yet |
| 03:33:53 | ~600 | GBuffer/Direct: Ray Query, Indirect: Trace Ray | Ray tracing modes selected |
| 03:33:58 | 1788 | **First camera cut** | **Camera detected — transform pipeline connected** |
| 03:34:44 | ~4000 | "Skipped drawcall, colour write disabled" | Shadow/depth pass correctly skipped |
| 03:34:51 | 4829 | 6 camera cuts in 200ms | Matrix instability burst |
| 03:34:55 | ~4900 | "Pushing more unique buffers than supported" | Buffer overflow — some geometry drops |
| 03:36:18 | 5871-5880 | **10 camera cuts in 345ms** | **Severe instability — every-frame cuts** |
| 03:37:51 | ~10000 | GBuffer mode changed to RGS | Remix mode switch (user interaction?) |
| 03:38:10 | 11531-11567 | 10 camera cuts | Another instability burst |
| 03:38:20 | 11740-11746 | 7 camera cuts | Continuing instability |
| 03:39:42 | ~14000 | Client process exited | Game closed (crash or manual) |

---

## 10. File Artifact Inventory

### Repository (patches/trl_legend_ffp/)

| File | Size | Purpose |
|------|------|---------|
| `proxy/d3d9_device.c` | ~25KB | Core FFP conversion + transform override |
| `proxy/d3d9_main.c` | ~8KB | DLL entry, logging, Remix chain-loading |
| `proxy/d3d9_wrapper.c` | ~4KB | IDirect3D9 wrapper |
| `proxy/proxy.ini` | ~200B | Runtime configuration |
| `proxy/build.bat` | ~2KB | MSVC x86 no-CRT build |
| `kb.h` | ~80 lines | Knowledge base (structs, function signatures, globals) |

### Game Directory (A:\SteamLibrary\...\Tomb Raider LegendFIRSTVIBECODE\)

| File | Size | Purpose |
|------|------|---------|
| `d3d9.dll` | 19,456B | Active FFP proxy (current build) |
| `d3d9.dll.bak` | ~2MB | Remix bridge client |
| `proxy.ini` | ~200B | Active proxy config |
| `rtx.conf` | ~8KB | Remix config (36 settings, 150+ texture hashes) |
| `user.conf` | ~1KB | Remix UI session state |
| `dxvk.conf` | ~15KB | DXVK options (all defaults/commented) |
| `dxwrapper.dll` + `.ini` | ~1MB | D3D8→D3D9 wrapper |
| `ffp_proxy.log` | ~512KB | Current session proxy log |
| `workspace_analysis.md` | ~3.9K lines | Previous comprehensive analysis |

### Game Directory — Reverse/

| Path | Contents |
|------|----------|
| `builds/` | 10 versioned DLLs (18-20KB proxy builds, 163KB tracer DLLs) |
| `configs/` | 4 archived proxy.ini/user.conf snapshots |
| `logs/ffp-proxy/` | 9 archived session logs |
| `logs/dx-trace/` | JSONL frame capture (148,696 calls) + tracer log |
| `logs/remix-runtime/` | Archived Remix metrics + NRC logs |
| `tests/` | 13 test snapshots (all "No" result) |

---

## 11. Risk Assessment & Prioritized Next Steps

### Highest Priority (Configuration Fixes — No Code Changes)

| # | Action | Why | Risk |
|---|--------|-----|------|
| 1 | Set `rtx.useVertexCapture = False` | Conflicts with FFP path, may cause dual geometry processing | None — purely additive |
| 2 | Find and remove `d3d9.apitraceMode = True` | Active despite being commented in dxvk.conf, changes buffer semantics | None |
| 3 | Test `rtx.skipDrawCallsPostRTXInjection = False` | May be silently killing FFP geometry | May cause visual artifacts from post-process passes |

### High Priority (Requires Testing)

| # | Action | Why | Risk |
|---|--------|-----|------|
| 4 | **Save current build as formal test** | The 3/17+ build has never been Yes/No validated | None |
| 5 | Negate projection Y-scale before SetTransform | `-2.28 → +2.28` may stabilize camera detection | May flip scene upside-down |
| 6 | Analyze camera cut storms | Determine if World matrix is camera-dependent (it shouldn't be) | Diagnostic only |

### Medium Priority (Code Changes)

| # | Action | Why | Risk |
|---|--------|-----|------|
| 7 | Route SHORT4 POSITION draws to passthrough | FFP can't handle SHORT4 natively | May reduce FFP geometry volume |
| 8 | Run `--shader-map` on frame trace JSONL | Definitively confirm CTAB register→name mapping | None |
| 9 | Enable `FrustumPatch=1` after base works | Required for off-screen geometry in reflections | Quality improvement only |

### Low Priority (Polish)

| # | Action | Why | Risk |
|---|--------|-----|------|
| 10 | Address "unique buffers" overflow | Some geometry dropping silently | May require Remix config tuning |
| 11 | Enable skinned geometry (`ForceFfpSkinned=1`) | Character rendering | Only after rigid geometry is confirmed |
| 12 | Investigate wireframe grid lines | Terrain patch boundary artifacts | Remix-side issue |

---

## 12. Summary Assessment

### What We Can See (Proxy Visibility)

The proxy has **excellent visibility** into the game's render pipeline:
- Every `SetVertexShaderConstantF` call is captured with register index, count, and float values
- Every `DrawIndexedPrimitive` is logged with vertex count, primitive count, stride, routing decision, active shaders, and textures
- Every vertex declaration change is parsed with element types and offsets
- Game memory matrices (View, Projection) are read directly at known addresses
- WVP decomposition produces three physically plausible matrices per draw

### What We Can See (Remix Side)

Remix confirms:
- Configuration is loaded and parsed correctly
- Camera becomes valid ~12 seconds after game start
- Camera tracking is active (68 cut events = Remix is processing transforms)
- Ray query and trace ray modes are both engaged
- Buffer overflow warning indicates geometry IS reaching Remix
- At least one draw call was correctly identified as shadow/depth pass

### What We Cannot See (Gap)

The critical unknown is **what Remix actually renders in the path-traced view**. The current build has never been tested with:
- Alt+X developer menu → wireframe/hash/normals debug views
- Visual confirmation of path-traced geometry
- Screenshot or recording of the rendered output

The proxy and Remix logs both show positive signals, but the human-in-the-loop visual validation step has not been completed for the current build.

### Overall Assessment

**The rendering pipeline is architecturally sound and end-to-end connected.** The proxy correctly decomposes TRL's fused WVP into separate transforms that Remix can consume. Camera detection works. The remaining blockers are likely:

1. `useVertexCapture = True` creating a dual-path conflict (highest probability)
2. `skipDrawCallsPostRTXInjection = True` killing FFP geometry (medium probability)
3. Camera instability from matrix decomposition precision (observable, needs investigation)
4. `d3d9.apitraceMode = True` ghost setting changing buffer behavior (possible contributor)

Three config toggles + a formal visual test could resolve this.

---

*End of deep analysis report.*
