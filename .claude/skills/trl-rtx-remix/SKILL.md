---
name: trl-rtx-remix
description: Tomb Raider Legend RTX Remix compatibility. Use when working on making Tomb Raider Legend (2006) render correctly under NVIDIA RTX Remix path tracing. Covers the full stack — dxwrapper D3D8→D3D9 translation, FFP proxy conversion, camera/matrix recovery, draw call routing, culling removal, fullscreen quad handling, geometry hash stability, denoiser grid artifacts, and Remix configuration. Builds on @dx9-ffp-port and @dynamic-analysis skills.
---

# Tomb Raider Legend — RTX Remix Compatibility

Make Tomb Raider Legend (TRL, 2006) render correctly under NVIDIA RTX Remix path tracing. This skill covers TRL-specific architecture, known pitfalls, and validated solutions. For general FFP proxy mechanics see @dx9-ffp-port; for live analysis see @dynamic-analysis.

---

## TRL Runtime Stack

```
trl.exe (D3D8 API calls)
  → dxwrapper.dll (D3D8 → D3D9 translation)
    → d3d9.dll (FFP proxy — our code)
      → d3d9_remix.dll (RTX Remix bridge/runtime)
```

**Critical fact**: TRL is natively Direct3D 9.0c internally (SM 2.0/3.0, DX9.0c system requirement), but it presents a D3D8 interface externally. The game uses dxwrapper (elishacloud) for D3D8→D3D9 translation. No crosire d3d8to9 wrapper is needed. The dxwrapper passthrough is already in the deployment pipeline.

The FFP proxy (`d3d9.dll`) sits between dxwrapper and Remix. It intercepts the D3D9 device that dxwrapper creates, converts shader-based draws to FFP, and chain-loads `d3d9_remix.dll`.

### Deployment Layout (Game Directory)

```
A:\SteamLibrary\steamapps\common\Tomb Raider Legend\
  trl.exe
  dxwrapper.dll          ← D3D8→D3D9 translation
  dxwrapper.ini          ← dxwrapper config
  d3d9.dll               ← FFP proxy (built from patches/trl_legend_ffp/proxy/)
  d3d9_remix.dll         ← RTX Remix runtime (renamed from Remix's d3d9.dll)
  proxy.ini              ← FFP proxy config
  rtx.conf               ← Remix runtime config
  .trex/                 ← Remix mod folder (USD replacements, rtx.conf overrides)
```

### Sync and Launch

Always sync before testing:
```bash
powershell -ExecutionPolicy Bypass -File "patches/trl_legend_ffp/sync_runtime_to_game.ps1"
# or with auto-launch:
powershell -ExecutionPolicy Bypass -File "patches/trl_legend_ffp/sync_runtime_to_game.ps1" -Launch
```

Working directory for trl.exe MUST be the game directory — the game resolves `bigfile.*` archives relative to CWD.

---

## The Core Problem: Shader-Based Rendering

TRL ships two render modes: a baseline fixed-function/SM 1.x path and a "Next Generation Content" SM 2.0/3.0 path. Both use programmable vertex shaders that upload matrices through `SetVertexShaderConstantF` — not through `SetTransform`. Remix requires `SetTransform` calls with correct separated World, View, and Projection matrices to reconstruct the 3D scene for path tracing.

**Without the FFP proxy**: Remix receives identity matrices from `SetTransform` (never called by the game), cannot reconstruct camera or geometry positions, and reports `Trying to raytrace but not detecting a valid camera`.

---

## Camera and Matrix Recovery

This is the hardest part of the TRL port. The game's matrix upload path goes through dxwrapper before reaching the proxy, and dxwrapper can flatten or reorder state in ways that lose matrix separation.

### Known Upload Architecture

- `FUN_00ECBA40` / `FUN_00ECBB00`: game-side helpers that call `SetVertexShaderConstantF`
- Callers: `FUN_0060c7d0`, `FUN_0060ebf0`, `FUN_00610850` — each serves a different render pass
- Observed upload patterns:
  - `start=0, count=4`: a changing 4×4 matrix (likely fused WorldViewProjection or View)
  - `start=6, count=1`: a scalar parameter (fog? clip?)
  - `start=8, count=8`: often ZERO on gameplay paths — this is the register range where View/Projection might be expected but isn't populated
  - `start=28, count=1`: another scalar

### Matrix Identification Strategy

1. **CTAB from shader disassembly** is the ground truth. Use `dx9tracer analyze --shader-map` on a captured frame to read named parameters from the shader bytecode constant tables. Look for names like `WorldViewProj`, `WorldView`, `ViewProj`, `World`, `Projection`.

2. **Const provenance** (`dx9tracer analyze --const-provenance-draw N`) shows which `SetVertexShaderConstantF` call populated each register at each draw. Cross-reference with CTAB to confirm register→matrix mapping.

3. **Const evolution** (`dx9tracer analyze --const-evolution vs:c0-c15`) reveals per-draw stability:
   - Registers that change every draw = World (per-object)
   - Registers stable across all draws in a frame but change between frames = View (camera)
   - Registers stable across frames = Projection (only changes on resize/FOV change)

4. **Live trace fallback**: if the tracer can't capture (dxwrapper interference), trace the game-side upload helpers:
   ```bash
   python -m livetools trace 0x00ECBA40 --count 50 \
       --read "[esp+8]:4:uint32; [esp+10]:4:uint32; *[esp+c]:64:float32"
   ```

### Fused vs Separated Matrices

TRL likely uploads a **fused WorldViewProjection** (WVP) in a single register block rather than separate W, V, P matrices. This is common in SM 2.0+ games.

**If the game uploads a fused WVP**: the proxy cannot trivially separate W from V×P. Options:

- **Find the source structs**: trace upstream callers to find where the game computes the WVP product. The individual W, V, P matrices exist in game memory before multiplication. Hook the computation point to capture them separately. This is the `map-camera-source` task in the camera pivot plan.

- **Use fusedWorldViewMode**: if only a fused W×V is available (not WVP), Remix's `fusedWorldViewMode` setting can handle it:
  - Mode 0: W, V, P all separate (ideal)
  - Mode 1: D3D9 View slot = fused W×V, World = identity
  - Mode 2: D3D9 World slot = fused W×V, View = identity

- **Decompose WVP at the proxy**: if the projection matrix is known (stable across frames), compute `W×V = WVP × P⁻¹`, then use fusedWorldViewMode=1 or 2. This requires capturing P once and inverting it.

### Remix Camera Validation

The proxy must make Remix stop reporting invalid camera. Check:

1. `SetTransform(D3DTS_VIEW, ...)` is called with a non-identity, non-zero matrix before draws
2. `SetTransform(D3DTS_PROJECTION, ...)` has a valid perspective projection (non-zero [0][0], [1][1], [2][2])
3. `rtx.fusedWorldViewMode` in rtx.conf matches the actual matrix layout the proxy provides
4. View matrix changes when the camera moves in-game
5. World matrix changes per-object (not per-frame)

**Symptom → Cause table for camera issues:**

| Symptom | Likely Cause |
|---------|-------------|
| "not detecting a valid camera" | View and/or Projection are identity/zero |
| Scene visible but camera doesn't track | View matrix is static (not updating from game camera) |
| Everything at origin / piled up | World matrix is identity for all objects |
| Geometry explodes / wobbles | fusedWorldViewMode mismatch; inverse transform uses wrong composition |
| Hash flickering in debug view | World matrix contains camera-dependent data (V or P leaked in) |

---

## Draw Call Routing — TRL Specifics

### Vertex Declaration Patterns

TRL uses multiple vertex formats. Key distinctions for routing:

| Format | Stride | Elements | Route |
|--------|--------|----------|-------|
| Rigid world geometry | ~24 bytes | POSITION + NORMAL + TEXCOORD | FFP convert |
| Skinned characters | ~32+ bytes | POSITION + BLENDWEIGHT + BLENDINDICES + NORMAL + TEXCOORD | Shader passthrough (until skinning enabled) |
| HUD / UI | varies | POSITION (no NORMAL), or POSITIONT | Shader passthrough |
| Fullscreen quads | varies | POSITIONT or large-triangle POSITION | Ignore / tag as UI |
| Particles | varies | POSITION + TEXCOORD (no NORMAL, non-indexed) | Evaluate: FFP or passthrough |

### Routing Adjustments for TRL

The default template routes by NORMAL presence: no NORMAL = HUD passthrough. TRL may have world geometry that **omits NORMAL** (e.g., alpha-blended decals, water surfaces, foliage). If world geometry is missing from the FFP path:

1. Check `ffp_proxy.log` for draws that got passthrough — look at their vertex declarations
2. If they have POSITION + TEXCOORD but no NORMAL, the `!curDeclHasNormal` filter is rejecting them
3. Options: remove the NORMAL filter entirely, or add a stride-based or texture-based heuristic

### Fullscreen Quad Detection

TRL's "Next Generation Content" mode uses post-processing passes (bloom, HDR tonemapping, color grading). These are fullscreen quads that **must not** be FFP-converted.

Detection in the proxy:
- POSITIONT / D3DFVF_XYZRHW in vertex format → screen-space, auto-passthrough
- Very low vertex count (4 or 6) with no depth test → likely fullscreen quad
- Texture matches a render target from a previous pass

Handling:
- Add texture hashes to `rtx.ignoreTextures` in rtx.conf for post-process passes Remix should skip
- Add texture hashes to `rtx.uiTextures` for overlays that should composite on top
- In the proxy, detect and skip these draws before FFP_Engage

---

## Culling

Path tracing needs geometry visible from all directions. TRL's engine culls aggressively.

### Game-Side Culling

TRL uses frustum culling and potentially BSP/portal visibility. The game-side culling must be disabled or replaced:

1. **Binary patch**: find the frustum cull function, NOP it out (`xor al, al; retn` = always visible)
2. **Distance-based replacement** (recommended): replace frustum cull with a box/sphere test centered on the player camera. Everything within radius N renders; everything outside is culled. This prevents the CPU bottleneck of rendering the entire level.

To find the cull function:
```bash
python -m retools.search "A:\SteamLibrary\steamapps\common\Tomb Raider Legend\trl.exe" strings -f "frustum,cull,visible,portal,vis" --xrefs
```

### Remix Anti-Culling (Supplements, Does Not Replace)

Configure in rtx.conf:
```ini
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2.0
rtx.antiCulling.object.farPlaneScale = 10.0
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.antiCulling.object.enableInfinityFarPlane = True
```

This retains geometry that was drawn at least once but left the frustum. It cannot force the game to submit geometry it already culled on the CPU side.

---

## Geometry Hash Stability

For asset replacement and instance tracking, Remix computes geometry and instance hashes. Unstable hashes manifest as **flickering colors** in the Geometry Hash debug view (Alt+X → Debug → Geometry Hash).

### Requirements for Stable Hashes

1. **World matrix must be camera-independent.** If the World matrix contains View or Projection data, the instance hash changes every frame. The proxy MUST provide only the object-to-world transform in `SetTransform(D3DTS_WORLD)`.

2. **Vertex buffers must be consistent.** If frustum culling rebuilds vertex buffers with different geometry subsets each frame, hashes will flicker. Disabling culling (above) fixes this.

3. **No LOD switching.** If the game switches between LOD meshes, hashes change. May need to force a single LOD level.

### Recommended Remix Settings

```ini
rtx.calculateMeshBoundingBox = True
rtx.antiCulling.object.hashInstanceWithBoundingBoxHash = False
```

---

## Grid Artifacts at Mesh Seams

Visible wireframe-like lines at mesh boundaries under path tracing. Not a named Remix bug — caused by the interaction of mesh topology with the NRD denoiser.

### Causes

1. **Normal discontinuities**: adjacent mesh pieces don't share vertex normals at seam vertices. NRD's edge-stopping function sees a sharp normal boundary and refuses to blend, leaving noisy/dark lines.

2. **T-junctions**: one triangle's edge meets the middle of another's edge. Causes light leaking and shadow artifacts invisible in rasterization.

3. **Auto-generated normals**: if the proxy strips normals and Remix auto-generates them per-submesh, discontinuities are guaranteed at mesh boundaries.

### Fixes

- **USD mesh replacements**: replace problematic meshes with properly welded geometry that shares vertex normals at seams. This is the definitive fix.
- **Denoiser tuning**: in Remix Rendering → Denoising, loosen normal thresholds (trades sharpness for fewer seam artifacts).
- **Normal debug view**: use Remix's Normals debug visualization to identify which meshes have discontinuities.
- **Backface culling for secondary rays**: `rtx.enableBackfaceCulling = True` reduces light bleeding at non-watertight seams.

---

## Remix Configuration Reference

### rtx.conf — Key Settings for TRL

```ini
# Camera / transform
rtx.fusedWorldViewMode = 0          # 0=separate W,V,P; 1=View=W×V; 2=World=W×V

# Anti-culling
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.fovScale = 2.0
rtx.antiCulling.object.farPlaneScale = 10.0
rtx.antiCulling.object.numObjectsToKeep = 10000
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True

# Hashing
rtx.calculateMeshBoundingBox = True

# Texture categorization (populate with actual hashes from the game)
rtx.ignoreTextures =                 # post-process / fullscreen quad textures
rtx.uiTextures =                     # HUD element textures
rtx.worldSpaceUITextures =           # in-world UI (health bars, etc.)

# Vertex capture (fallback only — prefer FFP conversion)
rtx.useVertexCapture = False         # True only if FFP path fails entirely
```

### proxy.ini — FFP Proxy Config

```ini
[Remix]
Enabled=1
DLL=d3d9_remix.dll

[FFP]
AlbedoStage=0                        # Which texture stage has the diffuse/albedo
```

---

## Diagnostic Workflow

### Phase 1: Does the proxy load and chain correctly?

1. Set `[Remix] Enabled=0` in proxy.ini
2. Launch the game — it should render normally with shaders (proxy is passthrough)
3. Check `ffp_proxy.log` exists in the game directory
4. If crash: check dxwrapper.ini is configured, dxwrapper.dll is present

### Phase 2: Does FFP conversion produce geometry?

1. Set `[Remix] Enabled=0`, keep proxy active
2. Wait 50s in-game, then check `ffp_proxy.log`
3. Look for `FFP ENGAGE` entries — these are draws being converted
4. Look for matrix values — are View/Proj non-zero? Are World matrices varying per-object?
5. If no FFP ENGAGE: check `viewProjValid` — the proxy isn't seeing valid matrices yet

### Phase 3: Does Remix see valid geometry?

1. Set `[Remix] Enabled=1`
2. Launch — Remix Developer Menu (Alt+X) should appear
3. Check for "not detecting a valid camera" in Remix log
4. Check Geometry Hash debug view — stable colors = good, flickering = hash instability
5. Check Wireframe debug view — geometry should match the game's level layout

### Phase 4: Does path tracing produce correct results?

1. Enable path tracing in Remix
2. Check for: missing geometry (culling), grid artifacts (normal discontinuities), floating quads (fullscreen post-process leaked into scene), wrong lighting direction (View matrix inverted or transposed incorrectly)

---

## SHORT4 Vertex Decompression

D3D9 FFP only supports `D3DDECLTYPE_FLOATn` and `D3DDECLTYPE_D3DCOLOR` vertex element types. If TRL uses `D3DDECLTYPE_SHORT4` for compressed vertex positions (common in console-era engines), the proxy must decompress before FFP submission:

```c
// SHORT4 → FLOAT4 decompression (typical scale+offset)
float x = (float)shorts[0] * scale + offset_x;
float y = (float)shorts[1] * scale + offset_y;
float z = (float)shorts[2] * scale + offset_z;
float w = 1.0f;
```

The scale and offset values are game-specific — check vertex shader disassembly for the `mad` instruction that decompresses SHORT4 positions. The CTAB may name it.

Check vertex element types in `ffp_proxy.log` or via `dx9tracer analyze --vtx-formats`.

---

## Files and Working State

| Path | Purpose |
|------|---------|
| `patches/trl_legend_ffp/proxy/d3d9_device.c` | Active FFP proxy source (edit this, not the template) |
| `patches/trl_legend_ffp/proxy/proxy.ini` | Proxy runtime config |
| `patches/trl_legend_ffp/kb.h` | Knowledge base: discovered functions, structs, register layout |
| `patches/trl_legend_ffp/sync_runtime_to_game.ps1` | Sync proxy build to game directory |
| `TOMB_RAIDER_LEGEND_RTX_REMIX_HANDOFF.md` | Full project history and prior failed approaches |
| `.cursor/plans/trl_camera_pivot_*.plan.md` | Camera recovery investigation plan |

### Key Addresses (from prior investigation)

These are documented in kb.h — always verify against the current binary:

| Address | Function | Role |
|---------|----------|------|
| `0x00ECBA40` | SetVSConstF helper | Wrapper that calls SetVertexShaderConstantF via dxwrapper |
| `0x00ECBB00` | SetVSConstF helper | Alternate upload path |
| `0x0060C7D0` | Render caller A | Calls ECBA40 — classify: gameplay camera, shadow, or auxiliary |
| `0x0060EBF0` | Render caller B | Calls ECBA40 — classify render pass type |
| `0x00610850` | Render caller C | Calls ECBA40 — classify render pass type |

---

## Decision Tree: What To Do Next

```
Is the proxy loading and chain-loading Remix?
├─ NO → Fix deployment: dxwrapper.dll, d3d9.dll, d3d9_remix.dll, proxy.ini
└─ YES
    Is Remix reporting "valid camera"?
    ├─ NO → Camera recovery:
    │   ├─ Capture frame with dx9tracer → --shader-map for CTAB register names
    │   ├─ Trace upstream callers (0x60c7d0, 0x60ebf0, 0x610850) to find
    │   │   where W, V, P exist separately before fusing
    │   ├─ Wire recovered matrices into FFP_ApplyTransforms
    │   └─ Set fusedWorldViewMode to match actual matrix layout
    └─ YES
        Is geometry visible in Remix debug views?
        ├─ NO → Draw routing: check ffp_proxy.log for FFP_Engage count,
        │       verify NORMAL filter isn't rejecting world geometry,
        │       check vertex element types for SHORT4 needing decompression
        └─ YES
            Is path tracing correct?
            ├─ Missing geometry → Culling: patch game-side frustum cull,
            │                     enable Remix anti-culling
            ├─ Grid artifacts → Normal discontinuities: USD mesh replacements,
            │                   denoiser tuning
            ├─ Floating white quads → Fullscreen post-process leaked:
            │                         add to rtx.ignoreTextures
            ├─ Hash flickering → World matrix has camera data:
            │                    ensure W is object-to-world only
            └─ Looks correct → Ship it
```
