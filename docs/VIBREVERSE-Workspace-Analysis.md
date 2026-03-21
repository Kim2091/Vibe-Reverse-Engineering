# Tomb Raider Legend × RTX Remix — Workspace Technical Analysis
*Generated 2026-03-19 from comprehensive file investigation*

---

## 1. Project Overview & Structure

### Goal
Make Tomb Raider Legend (2006, Crystal Dynamics, trl.exe) render under NVIDIA RTX Remix path
tracing by building a DX9 FFP proxy that converts the game's shader-based draw calls into the
fixed-function pipeline state Remix requires to reconstruct the 3D scene.

### Hardware / OS
- CPU: Intel Core i9-14900K
- GPU: NVIDIA GeForce RTX 5090 (32 GiB VRAM, driver 595.71.0, Vulkan 1.4.329)
- RAM: 63.72 GiB physical
- OS: Windows 10.0 Build 26200 (Windows 11 Home)
- Display: 3840×2160 @ 240 Hz

### Runtime Stack (as deployed)

```
trl.exe  (D3D8 API surface — but internally D3D9.0c)
  └─ dxwrapper.dll  (elishacloud D3D8→D3D9 translation, D3d8to9=1 mode)
       └─ d3d9.dll  (FFP proxy — our interceptor, built from patches/trl_legend_ffp/)
            └─ d3d9.dll.bak  (RTX Remix bridge client, remix-main+5a70985a)
                 └─ .trex/d3d9.dll  (dxvk-remix path tracer, DXVK Remix)
                      └─ .trex/NvRemixBridge64.exe  (64-bit Remix server process)
```

**Critical fact**: TRL is natively D3D9 SM 2.0/3.0 internally (DX9.0c system requirement),
but presents a D3D8 surface. dxwrapper handles the D3D8→D3D9 translation. The proxy sits
between dxwrapper and Remix.

### Directory Structure

```
Tomb Raider LegendFIRSTVIBECODE/
├── trl.exe                         ← game binary (never modified)
├── d3d9.dll                        ← active FFP proxy build
├── d3d9.dll.bak                    ← Remix bridge (renamed from Remix's d3d9.dll)
├── d3d9.pdb                        ← proxy debug symbols
├── proxy.ini                       ← FFP proxy config
├── rtx.conf                        ← Remix path-tracer config (36 active settings)
├── user.conf                       ← Remix UI session state
├── dxwrapper.dll / dxwrapper.ini   ← D3D8→D3D9 wrapper
├── ffp_proxy.log                   ← current session log (~512 KB, active)
├── .trex/                          ← Remix runtime DLLs + USD plugins
├── rtx-remix/logs/                 ← Remix bridge/dxvk logs
│   ├── bridge32.log                ← Remix bridge client (32-bit side)
│   ├── bridge64.log                ← Remix bridge server (64-bit side)
│   └── remix-dxvk.log             ← dxvk-remix path tracer (main Remix log)
├── Reverse/
│   ├── RULES.md                    ← workspace discipline rules
│   ├── builds/                     ← 10 versioned proxy DLLs + tracer DLLs
│   ├── configs/                    ← archived proxy.ini / user.conf snapshots
│   ├── logs/
│   │   ├── ffp-proxy/             ← 9 archived FFP proxy session logs
│   │   ├── dx-trace/              ← D3D9 frame JSONL capture + trace proxy log
│   │   └── remix-runtime/         ← archived Remix metrics + NRC session log
│   └── tests/                      ← 13 test snapshots (all result: No)
├── compass_artifact_wf-*.md        ← 3900+ line deep-dive technical analysis
└── trl-rtx-remix-SKILL.md          ← operational skill / decision-tree reference
```

---

## 2. Tools Built & Their Results

### FFP Proxy (d3d9.dll) — Main Tool

A custom D3D9 wrapper DLL (`d3d9.dll`) that sits between dxwrapper and Remix. Source lives in
`patches/trl_legend_ffp/proxy/d3d9_device.c` (not in this game directory; synced via
`sync_runtime_to_game.ps1`).

**What it does:**
- Wraps `IDirect3DDevice9` and `IDirect3D9`
- Intercepts `SetVertexShaderConstantF` to capture shader constant registers c0-c31
- Intercepts `DrawIndexedPrimitive` / `DrawPrimitive` to route eligible draws to FFP
- Performs WVP matrix decomposition to recover separate World, View, Projection matrices
- Calls `SetTransform(D3DTS_WORLD)`, `SetTransform(D3DTS_VIEW)`, `SetTransform(D3DTS_PROJECTION)`
  before each FFP draw so Remix can reconstruct the 3D scene
- Strips pixel shaders (`SetPixelShader(NULL)`) for FFP draws
- Disables normal-map texture stages (configurable via `DisableNormalMaps`)
- Chain-loads Remix bridge via `DLLName` in proxy.ini

**Build history (Reverse/builds/):**

| DLL Name | Date | Notes |
|---|---|---|
| d3d9.pre-trl-ffp.dll | 3/15 | Pre-FFP test build, 20 KB |
| d3d9.proxy-pre-20260315-211138.dll | 3/15 19:46 | Initial FFP, 17 KB |
| d3d9.proxy-pre-20260315-212108.dll | 3/15 21:11 | 18 KB |
| d3d9.proxy-pre-20260315-212611.dll | 3/15 21:21 | 18 KB |
| d3d9.proxy-pre-20260317-004753.dll | 3/17 00:47 | 18 KB — intermediate |
| d3d9.proxy-pre-20260317-020542.dll | 3/17 01:26 | 20 KB |
| d3d9.proxy-pre-20260317-023442.dll | 3/17 02:28 | 18 KB |
| d3d9.dll (current, game root) | 3/17+ | Most recent — the one being tested |

**Proxy config fields (proxy.ini):**
```ini
[Remix]
Enabled=1
DLLName=d3d9.dll.bak

[FFP]
AlbedoStage=0
DisableNormalMaps=0          ; currently 0 (was 1 in earlier builds)
ForceFfpSkinned=0
ForceFfpNoTexcoord=0
FrustumPatch=0               ; frustum matrix patch disabled
FrustumScaleMicros=100
```

### DX9 Frame Tracer (d3d9_trace.dll, d3d9_trace_endscene.dll)

Two tracer DLLs in Reverse/builds/ (163 KB each). These replace d3d9.dll to capture raw D3D9
API call sequences including shader bytecode disassembly (loaded from `d3dx9_43.dll`).

**Capture result** (Reverse/logs/dx-trace/dxtrace_proxy.log):
```
Config: CaptureFrames=2 CaptureInit=1 FrameBoundary=EndScene
Chain loading: ...\d3d9.dll.bak
Shader disasm: loaded from d3dx9_43.dll
=== CAPTURE STARTED ===
=== CAPTURE DONE ===
  Frames: 2  Calls: 148,696
```

**Capture output** (Reverse/logs/dx-trace/dxtrace_frame.jsonl, 35+ KB):
Frame -1 (init) contains CreateTexture, CreateVertexDeclaration, CreateVertexBuffer,
CreateIndexBuffer calls with full backtraces. Key init addresses in trl.exe:

| Address | Role in Backtrace |
|---|---|
| 0x00609A72 | Common renderer init entry |
| 0x005DD088 | Called from 0x00609A72 |
| 0x005DAC04 | Called from above |
| 0x0060AC65 | Outer game loop |
| 0x00EC67EF | D3D state manager |
| 0x0041977E | Game entry |

**Vertex declarations captured in init phase:**

| Handle | Elements | Stride | Usage |
|---|---|---|---|
| 0x018A8A68 | FLOAT3/POS + D3DCOLOR/TEX10 + FLOAT2/TEX | 24 | World rigid geometry |
| 0x018A8E58 | FLOAT3/POS + FLOAT2/TEX + FLOAT2/NORMAL | 28 | Alternate format |
| 0x018A9398 | FLOAT3/POS + D3DCOLOR/TEX10 | 16 | Position+color only |
| 0x018A92B8 | FLOAT3/POS + D3DCOLOR/TEX10 + FLOAT2/TEX + FLOAT2/TEX1 | 32 | Two texcoords |

---

## 3. Understanding of TRL's Rendering Pipeline

### Shader Architecture

TRL is D3D9 SM 2.0/3.0 internally. It ships two render modes:
- **Baseline path**: Fixed-function / SM 1.x compatibility
- **Next-Gen Content path**: SM 2.0/3.0 programmable shaders (used on modern hardware)

Both use `SetVertexShaderConstantF` — never `SetTransform`. This is the core problem: Remix
reads `SetTransform` state, TRL never calls it.

### Vertex Shader Constant Register Layout

From captured logs across sessions, the register map is:

| Registers | Content | Notes |
|---|---|---|
| c0–c3 (count=4) | Fused matrix — WVP or Projection-only | Changes per-draw; sometimes pure Proj |
| c4–c7 (count=4) | Partial data — (-0.00, 218.45, 0.00, 1.00) in row0 | Seen in 004753 log; purpose unclear |
| c6 (count=1) | Scalar (fog? clip distance?) | Single float4, seen in early sessions |
| c8–c15 (count=8) | TWO 4×4 matrices — appear to be bone matrices or ZERO | c8BlockNonZero=0 on rigid draws |
| c28 (count=1) | Scalar | |

**Critical discovery from 004753 build** (the earlier 3/17 log):
- `c8BlockNonZero=0`, `c12BlockNonZero=0` → these registers are ZERO on rigid world draws
- c0 sometimes = pure projection matrix (when no per-object transform is present)
- c0 sometimes = full WVP (most common for world geometry draws)

**Critical discovery from current build** (latest game-root log):
The current proxy detects two different matrix uploads at c0:
1. Pure projection (diagonal form, stable): renders UI/loading screen geometry
2. Full WVP (camera+object-dependent): renders world geometry

### The Projection Matrix

Stable across all sessions:
```
row0: [ 2.00,   0.00,  0.00,  0.00 ]
row1: [ 0.00,  -2.28,  0.00,  0.00 ]
row2: [ 0.00,   0.00,  1.00,  1.00 ]  ← W-pass-through (col3=1 = divide by z)
row3: [ 0.00,   0.00, -16.00, 0.00 ]  ← near plane = 16 world units
```

Interpretation: Right-handed infinite far plane perspective. X scale = 2.00 (narrower FOV
horizontal). Y scale = -2.28 (Y-flipped clip space, common in D3D9 Y-down convention).
Near plane = 16 units. Far plane → ∞. The negative Y scale is unusual and must be accounted
for when `SetTransform(D3DTS_PROJECTION)` is called — Remix expects D3D9 clip space convention.

### World Geometry — Matrix Decomposition (Current Build)

The current proxy performs `full decomp (W/V/P separate)` using the known stable projection:

```
WVP (at c0, per-draw example):
  row0: -1.22  0.86  0.00   58.13
  row1: -0.31 -0.44  2.60 9566.17
  row2: -0.56 -0.80 -0.20 -593.58
  row3: -0.56 -0.80 -0.20 -577.51

Decomposed World (translation per-object):
  row3: -31526.62, -2365.89, 58097.12, 1.00   (identity rotation)

Decomposed View (camera-dependent):
  row0: -0.61  0.13 -0.56  0.00
  row1:  0.43  0.19 -0.80  0.00
  row2:  0.00 -1.14 -0.20  0.00
  row3: -18308.28  66942.23  -8261.97  1.00

Decomposed Projection (stable):
  (same as above — 2.00 / -2.28 / near=16)
```

World-space coordinates are very large (tens of thousands of units). The world matrix
provides a per-object translation with near-identity rotation, which is correct behavior
for static world geometry.

### Observed Vertex Shader Handles

Five distinct shader handles observed across the session (from SetVS calls in ffp_proxy.log):

| Handle | Frequency | Role (inferred) |
|---|---|---|
| 0x214F9EB8 | Very high | Main world geometry shader |
| 0x019F1470 | High | Alternate world shader variant |
| 0x019F1290 | Medium | Another geometry variant |
| 0x214F9828 | Medium | Shader with different constant layout |
| 0x019F1560 | Rare | Possibly specialized pass |

(Note: These are runtime shader object pointers, not stable hashes — they change between sessions.)

### Key Binary Addresses (from trl-rtx-remix-SKILL.md + dxtrace backtraces)

| Address | Function | Role |
|---|---|---|
| 0x00ECBA40 | FUN_00ECBA40 | SetVertexShaderConstantF helper wrapper |
| 0x00ECBB00 | FUN_00ECBB00 | Alternate SetVSConstF upload path |
| 0x0060C7D0 | Render caller A | Calls 0xECBA40 — classify render pass |
| 0x0060EBF0 | Render caller B | Calls 0xECBA40 — classify render pass |
| 0x00610850 | Render caller C | Calls 0xECBA40 — classify render pass |
| 0x0060AC65 | Game loop | Main render frame entry |
| 0x005DD088 | Renderer sub | Called from game loop |
| 0x00609A72 | Init | Renderer initialization |
| 0x00EC8360 | D3D manager | D3D resource/state management |

---

## 4. What Works

### 4.1 Proxy Chain Loading
The proxy loads and chain-loads Remix correctly every session. Confirmed by:
- `ffp_proxy.log`: "Remix enabled, loading: ...\d3d9.dll.bak"
- `bridge32.log`: "Ack received! Handshake completed!"
- `bridge64.log`: "Server side D3D9 Device created successfully!"
- Device creation takes ~3 seconds (handshake + 64-bit server launch)

### 4.2 FFP Conversion — Draw Routing

The current proxy successfully routes world geometry draws to FFP:
```
DIP #352027  route=FFP  ffpActive=1  wvpValid=1  stride0=20  primCount=10   numVerts=10900
DIP #352028  route=FFP  ffpActive=1  wvpValid=1  stride0=20  primCount=14   numVerts=10900
DIP #352032  route=FFP  ffpActive=1  wvpValid=1  stride0=20  primCount=47   numVerts=25574
DIP #352033  route=FFP  ffpActive=1  wvpValid=1  stride0=20  primCount=78   numVerts=25574
```

Large vertex counts (10900–25574 verts) are world geometry, not trivial test draws.

### 4.3 Matrix Decomposition

The current build performs `full decomp (W/V/P separate)` — separating the fused WVP into
three distinct matrices before calling SetTransform. The decomposition produces non-identity,
physically plausible values:
- World: near-identity rotation + large translation (world-space object position)
- View: camera-dependent orientation matrix (changes with camera movement)
- Projection: stable 2.00/-2.28 perspective matrix (near=16, far=∞)

### 4.4 Remix Camera Detection — BREAKTHROUGH

The `remix-dxvk.log` from the most recent session shows:

```
[03:33:45.711] info: [RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera.
```

...but then starting at frame 1788 (approximately 12 seconds after launch):

```
[03:33:58.548] info: Camera cut detected on frame 1788
[03:33:59.528] info: Camera cut detected on frame 1845
[03:34:13.326] info: Camera cut detected on frame 2550
[03:34:13.376] info: Camera cut detected on frame 2551
...
[03:39:42.499] (game exited)
```

**This is the most significant progress to date.** "Camera cut detected" means Remix has
accepted the View matrix as valid and is tracking it frame-over-frame. The camera was NOT
valid at startup (main menu / loading screen) but BECAME valid once in-game. This confirms
the matrix decomposition approach is working.

Total camera cuts logged: 30+ events across a 6-minute session.

### 4.5 Texture Categorization (extensive, 150+ hashes)

`rtx.conf` contains well-developed texture categorization:
- `ignoreTextures`: 66 hashes (post-process passes, fullscreen quads, bloom, HDR)
- `uiTextures`: 22 hashes (HUD, menus, overlays)
- `skyBoxTextures`: 7 hashes (sky dome)
- `particleTextures`: 29 hashes (dust, rain, sparks, fire)
- `smoothNormalsTextures`: 17 hashes (surfaces needing smooth normals)
- `decalTextures`: 4 hashes
- `worldSpaceUiTextures`: 2 hashes (in-world UI elements)
- `animatedWaterTextures`: 1 hash
- `hideInstanceTextures`: 1 hash
- `raytracedRenderTargetTextures`: 1 hash (-0xE00529F572106A53)
- `worldSpaceUiBackgroundTextures`: 1 hash (-0x0B22CB80031DCB6C)

This texture categorization was built over multiple sessions of careful tagging and is
relatively mature.

### 4.6 Anti-Culling Configuration (working)

Remix anti-culling is enabled and configured:
```ini
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2
rtx.antiCulling.object.farPlaneScale = 10
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.antiCulling.object.enableInfinityFarPlane = True   (added from user.conf)
```

### 4.7 DX9 Frame Capture Infrastructure

The dx9 tracer (d3d9_trace.dll) successfully captured 2 frames / 148,696 calls with
shader bytecode disassembly. The JSONL output is available for further analysis with
`dx9tracer analyze --shader-map` or `--const-provenance`.

---

## 5. What Doesn't Work (Known Failures)

### 5.1 No Path-Traced Geometry — All 13 Saved Tests Tagged "No"

Every test snapshot in Reverse/tests/ is labeled "No" (Remix did not render path-traced
geometry). Tests span 3/14–3/15; the current build (3/17+) has not yet been saved as a test
with a Yes/No result.

### 5.2 Camera Instability — "Cut Storm" at Frames 5871–5880

The remix-dxvk.log shows 10 camera cuts in 9 consecutive frames (frames 5871–5880, between
03:36:18.430 and 03:36:18.775). Normal gameplay would not cause 10 camera cuts in ~350ms.
This strongly suggests:
- World matrix containing camera-dependent data (View bleeding into World)
- Hash/matrix instability at high draw-call counts
- OR a specific in-game animation/cutscene with many rapid camera changes

The camera cut storm likely causes RTX denoiser artifacts or hash flickering at that point
in the session.

### 5.3 SHORT4 Vertex Positions — Unconfirmed Handling

An early render pass uses `D3DDECLTYPE_SHORT4` for vertex positions (seen in both the
current ffp_proxy.log and the 004753 log):
```
DECL:
  [s0 +0] POSITION[0] SHORT4       ← compressed signed short positions
  [s0 +8] COLOR[0] D3DCOLOR
  [s0 +12] TEXCOORD[0] SHORT2
  [s0 +16] TEXCOORD[1] SHORT2
  stride=20
```

D3D9 Fixed-Function Pipeline does NOT support SHORT4 position elements. If the proxy is
routing these draws to FFP without decompressing SHORT4→FLOAT3, the geometry will be
misinterpreted by the GPU. It's unknown whether the current proxy handles this
decompression or passes through SHORT4 draws (likely the latter since stride=20 still
shows `route=FFP` in the logs — this could cause silent corruption).

The shader CTAB for these draws likely contains a `mad` instruction with a scale+offset
for decompression. Not yet extracted.

### 5.4 c8–c15 Block Is Zero for All Rigid Draws

The 004753 build shows `c8BlockNonZero=0` and `c12BlockNonZero=0` for every rigid geometry
draw. The c8–c15 range is what the proxy originally expected to contain separate View and
Projection matrices. These registers are never populated for non-skinned draws.

For skinned character geometry (not yet enabled), these registers likely contain bone matrices
(two 4×4 matrices = 8 float4 registers), not View/Projection.

### 5.5 fusedWorldViewMode=0 + useVertexCapture=True Conflict

Current rtx.conf has:
```ini
rtx.fusedWorldViewMode = 0         ; expect separate W, V, P
rtx.useVertexCapture = True        ; experimental fallback enabled
```

`fusedWorldViewMode=0` means Remix expects separate World, View, Projection from D3D9
SetTransform. The proxy IS providing this (full decomp). But `useVertexCapture=True` means
Remix also tries to capture raw vertex data from the GPU. The interaction of these two modes
is undefined and potentially conflicting — vertex capture bypasses the transform pipeline
entirely but may interfere with how Remix routes geometry.

The skill documentation warns: "vertex capture is experimental and fragile" with known race
conditions and "mesh explosions" (GitHub issues #245, #414).

### 5.6 Projection Matrix Y-Flip Issue

The projection matrix has a negative Y scale (-2.28). When passed to `SetTransform(D3DTS_PROJECTION)`,
this may cause Remix to see the scene as upside-down or reject it as invalid. Remix's camera
validation checks projection[1][1] sign against expected conventions. If Remix expects
positive Y in projection (standard D3D9 LH convention) and receives -2.28, camera validation
may intermittently fail.

This would explain why the camera is invalid at startup/menu but valid once in-game: the
menu may use one matrix layout while gameplay uses a different one, or the matrix upload
timing differs.

### 5.7 Game-Side Culling Not Patched

`FrustumPatch=0` in proxy.ini — the game's CPU-side frustum culling is NOT disabled.
Remix anti-culling retains previously-submitted geometry but cannot force the game to
submit geometry it culled on the CPU. Path-traced reflections and indirect lighting require
geometry visible from all angles, which the game's frustum culling prevents.

The trl-rtx-remix-SKILL.md documents the culling function search procedure but it has
not been performed.

---

## 6. Suspects for Rendering Issues

### Suspect 1 (MOST LIKELY): Camera Valid but Geometry Not Rendering Due to skipDrawCallsPostRTXInjection

```ini
rtx.skipDrawCallsPostRTXInjection = True
```

This setting tells Remix to skip all draw calls after a certain injection point. If the
injection point is misconfigured relative to the FFP proxy's injection point, Remix could
be skipping all the FFP-converted geometry draws. This setting was likely added to filter
out post-process passes, but it may be too aggressive.

### Suspect 2: Projection Y-Flip Causes Invalid Camera State

The -2.28 Y-scale in the projection matrix is atypical. If Remix's camera validation
rejects projection matrices with negative Y scale, the camera might be intermittently valid
and invalid. The "Camera cut detected at frame 1788" followed by the cut storm at frames
5871-5880 is consistent with the camera oscillating between valid and invalid states.

Try: negate the Y scale before calling SetTransform(D3DTS_PROJECTION) — use +2.28 and see
if the camera remains stable.

### Suspect 3: SHORT4 Vertices Corrupting FFP Path

If the proxy is sending SHORT4 vertex position data through the D3D9 FFP pipeline without
decompression, those draws will produce completely wrong geometry. Even though world geometry
uses FLOAT3, if the SHORT4 draws happen before world draws in the frame (they appear early
in the DIP sequence), they could corrupt shared GPU state or confuse Remix's instance tracker.

SHORT4 draws appear to be UI/HUD elements or a special render pass. They should be either:
a) Excluded from FFP conversion (routed as passthrough)
b) Decompressed to FLOAT3 by the proxy before submission

### Suspect 4: World Matrix Contains View Data (Hash Instability)

The camera cut storm at frames 5871-5880 could indicate that the World matrix (as set by
the proxy) is not camera-independent. If the decomposition algorithm has a bug where View
data leaks into the World matrix, every frame the camera moves would cause the instance
hash to change, generating camera cut events.

Check: Is the World matrix identical across consecutive frames for a static object? It should
be. The translation (-31526, -2365, 58097) seen in logs is for one specific object — but
if the view extraction fails mid-session, this value would start changing every frame.

### Suspect 5: useVertexCapture=True Interfering with FFP Path

Vertex capture mode and FFP mode should be mutually exclusive. With both enabled, Remix
may be double-processing geometry (once from FFP SetTransform, once from vertex capture),
leading to overlapping or flickering instances. The skill document recommends
`useVertexCapture = False` unless FFP fails entirely.

### Suspect 6: d3d9.apitraceMode = True (from dxvk.conf)

The remix-dxvk.log shows `d3d9.apitraceMode = True` is active. This is an apitrace
compatibility mode in DXVK that changes buffer update semantics. This setting was likely
needed for the tracer DLL but should NOT be active during normal FFP proxy testing. It
may alter vertex buffer upload behavior in ways that break vertex capture or FFP geometry.

---

## 7. Current State & Next Steps

### Current State (as of 2026-03-19)

The project is at a pivotal moment. The most recent build (post-20260317) has:
- ✅ Proxy loading and chain-loading correctly
- ✅ Matrix decomposition working (`full decomp (W/V/P separate)`)
- ✅ FFP conversion active (thousands of draws per frame routed to FFP)
- ✅ Remix detecting valid camera (camera cuts from frame ~1788 onward)
- ❓ Path-traced geometry rendering (not confirmed; test not yet saved)
- ❌ 13 previously saved tests all "No"

The session captured in the current logs ran 03:33:41–03:39:42 (6 minutes), ended with
"client process unexpectedly exited" (game crash or forced close).

The current test has NOT been saved to Reverse/tests/ with a Yes/No result.
Per RULES.md, this should be done immediately after testing.

### Immediate Next Steps (Priority Order)

**Step 1: Confirm current build result**
Save the current test: move `d3d9.dll` + `proxy.ini` + `ffp_proxy.log` to
`Reverse/tests/20260317-HHMMSS-<Yes|No>-MatrixDecompFullSplit/`.
The Yes/No depends on whether path-traced geometry was visible in the Remix Alt+X debug view.

**Step 2: Disable useVertexCapture**
In rtx.conf: `rtx.useVertexCapture = False`
Vertex capture conflicts with the FFP path. Since matrix decomposition is now working,
vertex capture is not needed and is likely causing interference.

**Step 3: Check d3d9.apitraceMode**
In dxvk.conf: verify `d3d9.apitraceMode` is False (or remove it). Apitrace mode should
only be active when running the tracer DLL.

**Step 4: Fix projection Y-flip**
In the proxy, before calling `SetTransform(D3DTS_PROJECTION)`, negate the [1][1] element
to get +2.28. This makes the projection match D3D9 LH convention. Monitor whether
"Camera cut detected" events become more regular and less frequent (fewer false cuts).

**Step 5: Exclude SHORT4 draws from FFP**
In the proxy, check vertex declaration for `D3DDECLTYPE_SHORT4` at POSITION[0] and route
those draws to shader passthrough (not FFP). These are not world geometry — they're UI or
special passes. Mark them for `rtx.uiTextures` or `rtx.ignoreTextures` based on their
texture hashes.

**Step 6: Extract shader CTAB**
Run `dx9tracer analyze --shader-map` on `Reverse/logs/dx-trace/dxtrace_frame.jsonl` to
get named parameters from the shader bytecode constant tables. This will definitively
confirm register→matrix mapping (WorldViewProj, View, Projection, etc.).

**Step 7: Validate geometry in debug views**
With the RTX Remix Alt+X developer menu:
- Wireframe debug view: confirm geometry layout matches level
- Geometry Hash debug view: check for flickering (hash instability)
- Normals debug view: check for discontinuities at mesh seams
- Camera validation: confirm not "not detecting valid camera" once in-game

**Step 8: Frustum culling patch**
Find the CPU frustum cull function using:
```bash
python -m retools.search trl.exe strings -f "frustum,cull,visible" --xrefs
```
Patch to NOP or replace with sphere-around-player test. This is required for path-traced
reflections/indirect lighting to work correctly.

**Step 9: Skinned character geometry**
Once rigid world geometry works: enable `ForceFfpSkinned=1` in proxy.ini and handle
bone matrix uploads (c8-c15 registers on skinned draws). This will add character rendering.

### Configuration Reference (Current rtx.conf Summary)

```ini
rtx.fusedWorldViewMode = 0          ; separate W/V/P mode
rtx.useVertexCapture = True         ; SHOULD BE FALSE — interferes with FFP
rtx.enableRaytracing = True         ; path tracing on
rtx.skipDrawCallsPostRTXInjection = True   ; CHECK IF TOO AGGRESSIVE
rtx.zUp = False                     ; Y-up coordinate system
rtx.orthographicIsUI = True         ; orthographic draws treated as UI
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2
rtx.antiCulling.object.farPlaneScale = 10
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.fallbackLightMode = 2           ; fallback lighting
rtx.fallbackLightRadiance = 10,10,10
rtx.renderPassGBufferRaytraceMode = 1
rtx.renderPassIntegrateDirectRaytraceMode = 1
rtx.hashCollisionDetection.enable = True
rtx.allowCubemaps = False
rtx.skyAutoDetect = 0
rtx.calculateMeshBoundingBox = True  ; (from user.conf)
rtx.antiCulling.object.enableInfinityFarPlane = True  ; (from user.conf)
```

---

## Appendix A — Test History

| Test Name | Date/Time | Config Focus | Result |
|---|---|---|---|
| 20260314-PISS-No-EarlyTest | 3/14 | Preliminary | No |
| 20260314-221922-No-LightBlue2 | 3/14 22:19 | Early texture filtering | No |
| 20260314-222624-No-FlashingLights | 3/14 22:26 | Flashing light avoidance | No |
| 20260315-020129-No-LightBlue | 3/15 02:01 | Texture filtering continued | No |
| 20260315-034534-No-FixedFunction | 3/15 03:45 | First FFP build, viewProjValid=0 | No |
| 20260315-034836-No-FlashingLights2 | 3/15 03:48 | Refined flash fix | No |
| 20260315-035208-No-Broken | 3/15 03:52 | Diagnostics | No |
| 20260315-040217-No-Broken2 | 3/15 04:02 | Diagnostics | No |
| 20260315-151703-No-TriangleSlices-AgentRestore | 3/15 15:17 | Triangle slicing test | No |
| 20260315-152226-No-LightBlue3-AgentPassthrough | 3/15 15:22 | Passthrough mode | No |
| 20260315-152906-No-FixedFunction2-AgentWorldview | 3/15 15:29 | fusedWorldViewMode=1+zUp=True | No |
| 20260315-154027-No-TriangleSlices2 | 3/15 15:40 | Triangle slicing refinement | No |
| (current 3/17 build) | 3/17+ | Full WVP decomp | TBD |

---

## Appendix B — Remix Build Info

- Remix version: `remix-main+5a70985a` (bridge + dxvk-remix)
- Build label: `rtx-remix-for-x86-games-1143-5a70985-debugoptimized` (March 2025)
- NRC: v0.13 (22 January 2025), Ultra preset
- Bridge: GUID-based IPC via shared memory segments

---

## Appendix C — Key Hex Values & Hashes

**Texture hashes (selected):**
- `0x443B45FB9971FC90` — skybox
- `0x95011A686BA05DFF` — animated water
- `0x1EFABA195948A63F` — hidden instance
- `-0xE00529F572106A53` — raytraced render target
- `0x040068FB4514ECAB` (+ its inverse `-0x040068FB4514ECAB`) — smooth normals surface

**Shader addresses (runtime, session-specific):**
- `0x214F9EB8` — most common world geometry VS
- `0x019F1470` — second most common
- `0x019F1290` — third
- `0x214F9828` — variant
- `0x019F1560` — rare

**Binary addresses (stable, from SKILL.md + dxtrace):**
- `0x00ECBA40` — SetVSConstF helper
- `0x00ECBB00` — alternate SetVSConstF
- `0x0060C7D0` / `0x0060EBF0` / `0x00610850` — render callers
- `0x0060AC65` — main game render loop

---

*End of workspace analysis. Next action: run the current build, check Alt+X debug view for
geometry/camera validity, save result to Reverse/tests/, then disable useVertexCapture.*
