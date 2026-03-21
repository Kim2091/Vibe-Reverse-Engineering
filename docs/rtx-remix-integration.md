# RTX Remix Integration with Direct3D 9

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [DLL Interposition and Bridge System](#dll-interposition-and-bridge-system)
3. [Scene Reconstruction from D3D9 Calls](#scene-reconstruction-from-d3d9-calls)
4. [The Dual-Hash System](#the-dual-hash-system)
5. [Hash Instability Causes and Fixes](#hash-instability-causes-and-fixes)
6. [Light Conversion and Stage Lights](#light-conversion-and-stage-lights)
7. [Game Compatibility Analysis](#game-compatibility-analysis)
8. [Tomb Raider Legend Case Study](#tomb-raider-legend-case-study)
9. [Configuration Reference](#configuration-reference)
10. [Modding Workflow](#modding-workflow)
11. [Toolkit API and MCP Server](#toolkit-api-and-mcp-server)

---

## Architecture Overview

RTX Remix is an open-source modding platform that intercepts DirectX 8/9 fixed-function pipeline games and replaces their rasterized rendering with full path tracing. It identifies every game object via deterministic hashes computed from geometry and texture data, enabling modders to replace meshes, materials, and lights at runtime without touching game code.

The platform targets **fixed-function pipeline games exclusively**. Games using vertex/pixel shaders extensively cannot have their rendering properly intercepted.

Repository: `NVIDIAGameWorks/rtx-remix` (monorepo with submodules)
- `dxvk-remix` — renderer + bridge code (merged as of early 2025)
- `toolkit-remix` — Omniverse-based creator application
- Build: Meson + Ninja, MSVC v142 (Visual Studio 2019)

## DLL Interposition and Bridge System

RTX Remix works by replacing the system `d3d9.dll` with its own. Three components operate across two processes:

**Bridge Client** (32-bit `d3d9.dll` next to game .exe):
- Runs inside the game's 32-bit process
- Intercepts ALL `IDirect3D9` / `IDirect3DDevice9` calls
- Serializes them into shared-memory IPC command queues
- Never calls the real system D3D9 runtime

**Bridge Server** (`NvRemixBridge.exe`, 64-bit):
- Receives serialized D3D9 commands via circular buffer IPC
- `AtomicCircularQueue` and `BlockingCircularQueue` for commands
- `SharedHeap` for large data (textures, vertex buffers)
- Handshake: SYN → ACK → CONTINUE with named semaphores
- Handle table maps 32-bit client handles to 64-bit server pointers

**DXVK-Remix Renderer** (64-bit `d3d9.dll` in `.trex/` folder):
- Fork of DXVK that translates D3D9 FFP calls into Vulkan
- Instead of rasterization, reconstructs scene and path-traces it
- Outputs via BLAS/TLAS acceleration structures → RTXDI → ReSTIR GI → NRD → DLSS

This architecture means RTX Remix sees exactly the same D3D9 API calls that a real `d3d9.dll` would. It must understand and reinterpret these calls to build a 3D scene.

## Scene Reconstruction from D3D9 Calls

The Scene Manager (`src/dxvk/rtx_render/`) intercepts specific D3D9 fixed-function calls:

| D3D9 Call | What Remix Extracts |
|-----------|-------------------|
| `SetTransform(D3DTS_WORLD, ...)` | Object position/orientation in world space |
| `SetTransform(D3DTS_VIEW, ...)` | Camera position and orientation |
| `SetTransform(D3DTS_PROJECTION, ...)` | Camera FOV, near/far planes |
| `SetLight(index, D3DLIGHT9*)` | Light type, position, direction, color, range, attenuation |
| `LightEnable(index, TRUE/FALSE)` | Which lights are active |
| `SetMaterial(D3DMATERIAL9*)` | Surface reflectance properties |
| `SetTexture(stage, texture)` | Texture bindings for material identification |
| `DrawPrimitive()` / `DrawIndexedPrimitive()` | Geometry data (vertices, indices) |
| `SetRenderState()` | Alpha test/blend settings, culling, etc. |
| `SetTextureStageState()` | FFP texture blending configuration |

**D3D9 Light → Remix Light Conversion:**
- D3D9 `D3DLIGHT_POINT` → Remix Sphere Light
- D3D9 `D3DLIGHT_DIRECTIONAL` → Remix Distant Light
- D3D9 `D3DLIGHT_SPOT` → Remix Sphere Light with light shaping (cone angle)

**D3D9 Material → Remix PBR Material Conversion:**
- `D3DMATERIAL9.Diffuse` → base color approximation
- `D3DMATERIAL9.Specular` + `Power` → roughness/metallic approximation
- `D3DMATERIAL9.Emissive` → emissive material flag
- Bound textures → texture hashes for PBR replacement matching

**Why shaders break this:**
When a vertex shader is active, `SetTransform()` has no effect — transforms are shader constants that Remix cannot interpret. When a pixel shader is active, `SetTextureStageState()` is irrelevant — material blending is in opaque shader code. Remix literally cannot reconstruct the scene because the rendering instructions are inside compiled shader bytecode rather than exposed through the state-based FFP API.

## The Dual-Hash System

RTX Remix computes three hash types for every draw call:

### Geometry Generation Hash (Instance Tracking)
- **Purpose:** Track draw call instances across frames for temporal algorithms (denoising, motion vectors)
- **Default components:** `positions + indices + texcoords + geometrydescriptor + vertexlayout + vertexshader`
- **Config:** `rtx.geometryGenerationHashRuleString`
- Must be stable frame-to-frame for path tracer temporal coherence

### Geometry Asset Hash (Replacement Matching)
- **Purpose:** Match captured game geometry with replacement USD assets
- **Default components:** `positions + indices + geometrydescriptor`
- **Config:** `rtx.geometryAssetHashRuleString`
- Deliberately excludes texcoords, vertexlayout, vertexshader for stability
- In `.usda` files: meshes named `mesh_XXXXXXXXXXXXXXXX` (hex of this hash)

### Texture Hash
- **Purpose:** Identify textures for PBR replacement and categorization
- **Method:** Content-based hash of raw pixel data
- Deterministic — same texture data always produces same hash
- Used to categorize textures (UI, Sky, Particles, Decals, Terrain, Water, Ignore) in `rtx.conf`

## Hash Instability Causes and Fixes

Hash instability is the single biggest practical challenge in RTX Remix modding. Five primary causes:

### 1. Game Culling Mechanisms (Most Common)
Frustum culling, PVS, or portal-based culling reorganizes which geometry is submitted each frame, splitting vertex/index buffers differently.

**Fixes:**
- `rtx.antiCulling.object.enable = True` — extend object lifetime beyond frustum
- `rtx.antiCulling.object.enableHighPrecisionAntiCulling = True` — SAT-based intersection
- Community compatibility mods that patch the game engine to disable culling entirely

### 2. CPU-Side Software Skinning
Games performing skeletal animation on CPU modify vertex positions before GPU submission.

**Fixes:**
- `rtx.calculateAABB = True` — per-draw-call bounding boxes improve tracking
- Anchor asset technique for affected meshes

### 3. Dynamic Vertex Buffers
Particle systems, physics meshes, procedural geometry use changing buffers.

**Fix:** Generally must be excluded or handled via texture-based identification.

### 4. Vertex Color Baked Lighting
Per-frame lighting baked into vertex colors.

**Fix:** `rtx.ignoreVertexColor = True` — excludes vertex colors from hash computation.

### 5. Draw Call Batching Variations
Game engines batch geometry differently between frames.

**Fix:** Adjust hash rule strings to exclude variable components.

### Verifying Hash Stability
Developer menu (Alt+X) → Rendering → Debug → "Debug View" → "Geometry Hash"
- Stable colors = stable hashes = reliable replacement
- Flickering colors = instability requiring workarounds

### Anchor Asset Technique
For unstable world geometry:
1. Identify a stable, non-culled mesh as an "anchor"
2. Remove the unstable-hash asset from the mod layer
3. Append the anchor as a stand-in
4. Transform it to match the original's position
5. Attach replacement geometry to it

## Light Conversion and Stage Lights

### Remix Light Types (Ranked by Efficiency)
1. **Sphere** — best general-purpose, replaces D3D9 point lights
2. **Rectangular** — panel lights, windows
3. **Disk** — circular flat emitters
4. **Cylinder** — tube lights
5. **Distant** — sun/moon, replaces D3D9 directional lights

All support: position, rotation, scale, color, color temperature, exposure, intensity (radiance units).
Sphere, disk, rect additionally support: light shaping (cone angle, softness, focus).

### Stage Lights (User-Added)
"Stage light" = user-added primitive light placed in the USD stage (as opposed to auto-converted D3D9 lights).

**Creation:** Select mesh → "Add new stage light…" → choose type. Light is created as a child prim of the selected mesh.

**Anchoring to Moving Geometry:**
The parent-child USD relationship is key. Child prims inherit parent transforms. When a mesh moves at runtime (because the game's draw call transform updates via `SetTransform(D3DTS_WORLD, ...)`), the light moves with it. The light's transform is in object-space relative to the parent mesh.

**Critical requirement:** Set `preserveOriginalDrawCall = 1` on the mesh prim when adding lights, otherwise the original mesh disappears.

**USD hierarchy:**
```
/RootNode/meshes/mesh_HEXHASH/     ← mesh prim (identified by hash)
    /mesh                           ← actual geometry
    /SphereLight                    ← stage light attached to this mesh
```

Changes must target `mesh_HASH` prims, not `inst_HASH_x` prims (instance prims are references).

### Anti-Culling for Lights
- `rtx.antiCulling.light.enable` — prevent lights from disappearing when game culls associated geometry
- `rtx.antiCulling.light.fovScale` — anti-culling frustum scale
- `rtx.antiCulling.light.numFramesToExtendLightLifetime` — persistence frames after game stops drawing

## Game Compatibility Analysis

### Compatible Games (Pure FFP or Mostly FFP)
Games using D3D8 or D3D9 with fixed-function T&L and texture stages. Examples:
- Morrowind, Half-Life 2 (early builds), Portal (original), many pre-2004 titles
- Games with "simple" vertex shaders may work with VS Capture mode

### Incompatible Games (Shader-Heavy)
Games using SM 2.0/3.0 vertex and pixel shaders for core rendering:
- Tomb Raider: Legend, most post-2005 D3D9 titles
- Remix hooks but cannot take over rendering
- No `rtx.conf` generated = Remix couldn't establish rendering takeover

### Compatibility Mod Approach (xoxor4d Method)
For shader-based D3D9 games, community developers create ASI/DLL hooks that:
1. Intercept the game's shader-based draw calls
2. Reimplement the rendering through FFP calls (SetTransform, SetLight, SetMaterial, SetTexture, DrawPrimitive)
3. Feed the reimplemented FFP calls to RTX Remix

Working compatibility mods exist for: BioShock 1, Black Mesa, SWAT 4, FEAR 1, Guitar Hero 3, NFS Carbon, GTA IV. The `ue2fixes` universal patcher handles Unreal Engine 2–2.5 games.

### Compatibility Checklist
1. Does the game use `d3d9.dll` (not d3d8, d3d10+, OpenGL)?
2. Does it use fixed-function vertex processing for majority of rendering?
3. Does it avoid extensive pixel shader usage for base rendering?
4. If shaders used, are they simple enough for experimental VS capture?
5. If no to 2–4, is a compatibility mod available or feasible?

## Tomb Raider Legend Case Study

**Status: Officially Incompatible** (GitHub Issue #287, closed as "incompatibility")

Tomb Raider: Legend (Crystal Dynamics, 2006) is a DirectX 9.0c game using programmable vertex and pixel shaders. Two rendering paths exist:
- **Base mode:** Still uses some shaders beyond pure FFP T&L
- **"Next Generation Content" mode:** SM 2.0/3.0 shaders (normal mapping, real-time shadows, DOF, water)

**Test results (RTX 2060, Steam version):**
- Remix hooks successfully, Alt+X developer menu appears
- NO visual changes occur in-game — Remix cannot intercept shader rendering
- With NGC mode: some materials go missing (partial interception, nothing usable)
- No `rtx.conf` generated

**Community FFP fallback attempts:**
- Registry edits under `HKEY_CURRENT_USER\Software\Crystal Dynamics\Tomb Raider\Graphics` for `RenderAPI` and `Shader Model 3.0` settings
- No confirmed success

**DXVK issues compound the problem:**
- DXVK Issue #4319: flickering from unhandled render state 181 (`D3DRS_MULTISAMPLEANTIALIAS`) and buffer creation failures
- Fixed in DXVK PR #4442, but underlying shader incompatibility remains

**Current best alternative:** "Tomb Raider Legend Care Package" on NexusMods (ReShade + dgVoodoo2, not RTX Remix).

**Path forward requires one of:**
1. A compatibility mod reimplementing rendering through FFP (xoxor4d approach)
2. Expansion of Remix's shader capture capabilities
3. Game engine modifications

## Configuration Reference

### Core Config Files
- **`rtx.conf`** — game compatibility (texture categories, hash rules, render categories)
- **`user.conf`** — personal graphics (DLSS, frame gen)
- **Logic-driven .conf layers** — dynamic runtime transitions

### Key Settings for Game Compatibility
```ini
# Hash stability
rtx.geometryAssetHashRuleString = positions,indices,geometrydescriptor
rtx.geometryGenerationHashRuleString = positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader
rtx.ignoreVertexColor = True
rtx.useBuffersDirectly = True
rtx.preserveDiscardedTextures = True
rtx.calculateAABB = True

# Anti-culling
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.enableHighPrecisionAntiCulling = True
rtx.antiCulling.light.enable = True
rtx.antiCulling.light.numFramesToExtendLightLifetime = 100
rtx.antiCulling.light.fovScale = 2.0

# Shader workarounds
rtx.useWorldMatricesForShaders = True  # if game sets world matrices alongside shaders

# Texture categories (hex texture hashes)
rtx.textureCategories.ui = <hash1>,<hash2>,...
rtx.textureCategories.sky = <hash1>,...
rtx.textureCategories.particles = <hash1>,...
rtx.textureCategories.decals = <hash1>,...
rtx.textureCategories.terrain = <hash1>,...
rtx.textureCategories.water = <hash1>,...
rtx.textureCategories.ignore = <hash1>,...
```

### Auto-Generated Reference
Run Remix with environment variable `DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1` to generate the complete `RtxOptions.md` reference in the `dxvk-remix` directory.

## Modding Workflow

### Six Phases
1. **Runtime Setup:** Copy `d3d9.dll` + `.trex/` next to game .exe → launch → Alt+X → tag textures → verify hash stability
2. **Capture:** Navigate to area → disable "Enable Enhanced Assets" → "Capture Scene" → exports to USD in `rtx-remix/captures/`
3. **Project Setup:** Open Toolkit → create project → point to `rtx-remix` dir → select capture → symlink created to `rtx-remix/mods/`
4. **Asset Authoring:** Create replacements in DCC tools → ingest → replace captured assets → adjust transforms → assign PBR materials → add lights
5. **Testing:** Launch game → mod loads via symlink → toggle replacements independently
6. **Packaging:** Mod Packaging tab → convert USDA to binary USD

### USD Stage Structure
- **Capture layer** (read-only base data) — original game geometry/materials/textures
- **Mod layer** (`mod.usda`) — all overrides (replacements, additions, lights)
- Sublayers for organization (by chapter, asset type, team member)
- Parent layers override child layers per USD composition rules

## Toolkit API and MCP Server

The RTX Remix Toolkit exposes:
- **REST API** — programmatic control of toolkit operations
- **MCP Server** — auto-starts at `http://127.0.0.1:8000/sse` using Server-Sent Events
  - Translates REST endpoints into MCP-compatible tools for LLM tool calling
  - Capabilities: asset replacement, metadata updates, light placement, mod interactions

**NVIDIA Langflow Template:**
- RAG module (documentation-embedded) for informational queries
- Action module (MCP-connected) for toolkit commands
- MCP Prompts provide predefined workflow templates

**Remix Logic System** (January 2026):
- No-code node-based UI for dynamic light/rendering behavior
- 30+ game event triggers (camera state, bounding boxes, key presses, mesh proximity)
- Controls 900+ graphical settings including light properties
- Logic graphs attached to Light or Mesh prims

---

## Key External References
- RTX Remix GitHub: `https://github.com/NVIDIAGameWorks/rtx-remix`
- DXVK-Remix: `https://github.com/NVIDIAGameWorks/dxvk-remix`
- TR Legend Issue #287: `https://github.com/NVIDIAGameWorks/rtx-remix/issues/287`
- Anchor Asset Issue #388: `https://github.com/NVIDIAGameWorks/rtx-remix/issues/388`
- xoxor4d compatibility mods: `https://github.com/xoxor4d`
