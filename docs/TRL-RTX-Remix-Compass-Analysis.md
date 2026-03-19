# Making Tomb Raider Legend work with RTX Remix

**Tomb Raider Legend is a shader-based D3D9 game — not D3D8 — and making it compatible with RTX Remix requires reimplementing its rendering as fixed-function pipeline calls, providing correct separated transform matrices, disabling game-side culling, and carefully handling screen-space draws.** This report synthesizes findings from the DXVK-Remix source code, NVIDIA documentation, GitHub issues, and the RTX Remix modding community (particularly xoxor4d's compatibility mod framework) to validate or invalidate the specific technical approaches under consideration.

A critical preliminary correction: despite the premise that TR Legend is a D3D8 game, all evidence — system requirements (DX9.0c, SM 2.0/3.0 GPUs), DXVK bug reports tagged `[d3d9]`, WikiRaider documentation of SM 2.0/3.0 render modes, and the absence of any D3D8 code path — confirms **Legend is natively Direct3D 9**. No d3d8-to-d3d9 wrapper is needed. The game ships a standard D3D9 renderer with two modes: a baseline fixed-function/SM 1.x path and a "Next Generation Content" SM 2.0/3.0 path.

---

## How vertex capture intercepts and reverse-maps geometry

RTX Remix's `rtx.useVertexCapture = True` is an **experimental feature** for games using programmable vertex shaders. When enabled, Remix intercepts the post-vertex-shader output — vertices already transformed to **clip/homogeneous coordinates** — and then uses the `SetTransform` matrices (World, View, Projection) stored on the D3D9 device to **reverse-map those vertices back to world space** for path tracing scene reconstruction.

The pipeline works as follows: the game's vertex shader transforms vertices from object space to clip space (applying World × View × Projection). Remix captures these clip-space positions, then computes the inverse of the View × Projection transform (or World × View × Projection, depending on `fusedWorldViewMode`) to recover world-space positions. In **v1.2.4**, NVIDIA improved precision by performing the perspective divide earlier in this pipeline, which fixed vertex "wobbling" and "explosions" in games like OutRun 2006: Coast 2 Coast.

**The critical requirement**: even though the game's vertex shader handles all transformation internally via shader constants, `SetTransform()` must still be called with correct, separate World, View, and Projection matrices before each draw call. Remix intercepts these calls and stores the matrices for its inverse-transform computation. If `SetTransform` is never called (common in shader-only games), Remix receives identity matrices and **cannot reconstruct the 3D scene**. A proxy/wrapper layer must inject the correct matrices by extracting them from the game's shader constants.

The vertex capture path has known stability issues. GitHub issue #245 documented a race condition where vertex positions from multiple objects were confused, causing "mesh explosions." Issue #414 reported large stray triangles from vertex memory corruption. Both are regressions between specific DXVK-Remix builds, indicating the feature remains fragile.

---

## fusedWorldViewMode controls how Remix decomposes transforms

The `fusedWorldViewMode` setting tells Remix how the game provides its transformation matrices through three modes:

- **Mode 0 (None)**: World, View, and Projection are provided **separately** via `SetTransform`. Remix composes them as W × V × P internally. This is the correct mode when a proxy layer injects properly separated matrices.
- **Mode 1 (View)**: The D3D9 View matrix slot contains a **fused World × View** matrix. The World matrix is treated as identity. Use this when the game pre-multiplies World into View on the CPU.
- **Mode 2 (World)**: The D3D9 World matrix slot contains a **fused World × View** matrix. The View matrix is treated as identity.

**If `fusedWorldViewMode = 1` but View and Projection are identity**, Remix concludes there is no camera transform at all. The inverse transform becomes identity, so captured clip-space vertices are not properly transformed back to world space. Geometry collapses or appears at the origin. The camera position and orientation become undefined. This is a **critical misconfiguration**.

A notable finding: `fusedWorldViewMode` was **not found in the current RtxOptions.md** auto-generated documentation. It may be an internal/unlisted option, renamed in recent builds, or part of the DXVK-level `d3d9.conf` configuration rather than the RTX-specific `rtx.conf`. Modders should verify its presence in their specific Remix runtime version.

For Tomb Raider Legend, the recommended approach is **mode 0** with separate matrices extracted from the game's shader constant registers and injected via `SetTransform` before each draw call.

---

## Geometry hashing requires camera-independent World matrices

RTX Remix computes geometry hashes to uniquely identify meshes for asset replacement, frame-to-frame instance tracking, and USD scene capture. The hash system operates at multiple levels: **geometry hash** (based on vertex positions, normals, UVs, and index data), **surface hash** (incorporates bound textures), and **instance hash** (incorporates the World matrix to distinguish different placements of the same mesh).

**The World matrix directly affects instance hashing.** If the World matrix contains camera-dependent data — such as a pre-multiplied World × View × Projection — the instance hash changes every frame as the camera moves, making the object unreplaceable and untrackable. In the Geometry Hash debug view (Alt+X → Debug → Geometry Hash), unstable objects show **flickering/changing colors** instead of stable solid colors.

Common causes of hash instability beyond incorrect matrix decomposition include: frustum/occlusion culling rebuilding vertex buffers with different geometry subsets each frame, CPU-side skinning modifying vertex data per frame, LOD switching, and dynamic vertex buffer streaming. NVIDIA's documentation recommends using **Anchor Assets** — stable, non-culled reference objects — as a workaround when hashes cannot be stabilized.

The recommended configuration for vertex-captured games includes enabling `rtx.calculateMeshBoundingBox = True`, which computes an axis-aligned bounding box for every draw call and "may improve instance tracking across frames for skinned and vertex shaded calls." Additionally, `rtx.antiCulling.object.hashInstanceWithBoundingBoxHash` should be disabled if primitive culling causes flickering.

**The core principle is absolute**: the World matrix passed to `SetTransform(D3DTS_WORLD)` must contain **only the object-to-world transform** — never any camera-dependent View or Projection data.

---

## Tomb Raider Legend is D3D9, not D3D8 — implications for the approach

Multiple sources confirm TR Legend (2006) is **natively Direct3D 9.0c**: DXVK issue #4319 shows D3D9 API calls in logs, WikiRaider documents SM 2.0/3.0 render modes, system requirements specify DX9.0c, and the minimum GPU (GeForce 3Ti) was accessed via D3D9 APIs for its fixed-function TnL. **No D3D8 render path exists in the game.** This eliminates the need for any d3d8-to-d3d9 wrapper (dxwrapper or crosire's d3d8to9).

For context on actual D3D8 games using RTX Remix: dxwrapper (elishacloud) is preferred over crosire's d3d8to9 for its better error handling, verbose logging, and process exclusion features. NVIDIA investigated switching their bundled wrapper (internal tracker REMIX-3034). The passthrough behavior of these wrappers simply forwards `SetTransform` calls — if the D3D8 game uses FFP transforms properly, the passthrough works. If the game is shader-driven and never calls `SetTransform`, identity matrices break Remix.

For TR Legend specifically, the approach should be the standard D3D9 `d3d9.dll` replacement. The game's **shader-based rendering** (SM 2.0/3.0) is the primary compatibility challenge, not any API version mismatch. NVIDIA's own compatibility guidance states: "DirectX 9.0c games are usually mostly shader based, so probably won't work" and "games released between 2000 and 2005 are most likely to work." TR Legend (April 2006) falls just outside the ideal window.

---

## FFP proxy conversion is the proven community approach

The RTX Remix modding community's **unanimous recommendation** for shader-based games is to reimplement rendering as fixed-function pipeline calls. This is the approach used by every successful shader-game Remix mod:

- **xoxor4d's remix-comp-projects** framework: covers Black Mesa, Bioshock 1, FEAR 1, GTA IV, NFS Carbon, Portal 2, and more — all reimplementing FFP rendering with game hooking via ASI loaders and MinHook
- **Deus Ex Echelon Renderer**: a complete FFP renderer replacing the Unreal Engine rendering plugin
- **Source Engine games**: use `-dxlevel 70-81` to force fixed-function mode, or complete FFP reimplementations for newer titles

The tradeoffs are clear. FFP conversion provides full Remix compatibility (geometry capture, material replacement, stable hashing, USD capture) but requires significant engine-level modification work and loses shader-dependent visual effects. Vertex shader capture avoids modification but is "very limited" per NVIDIA, has race condition bugs, and cannot handle pixel shader output decomposition into base color/roughness/normal — Remix needs raw albedo, not lit/shaded output.

**SHORT4 (D3DDECLTYPE_SHORT4) vertex positions cannot be handled by the FFP path.** The D3D9 fixed-function pipeline only supports `D3DDECLTYPE_FLOATn` and `D3DDECLTYPE_D3DCOLOR` data types for vertex elements. SHORT4 is a compressed format requiring vertex shader math to decompress (typically scale + offset). The Remix SDK's `remixapi_HardcodedVertex` uses static float positions. If TR Legend uses SHORT4 packed vertices, a proxy layer must **decode SHORT4 → FLOAT3/FLOAT4** before submitting geometry through the FFP path. Alternatively, vertex capture could handle the decompression via the original vertex shader, but with all the instability caveats mentioned above.

---

## Grid artifacts likely stem from normal discontinuities and denoiser behavior

No specific GitHub issue describes "wireframe grid lines at mesh seam boundaries" as a named bug. However, the architecture of RTX Remix's path tracing and denoising pipeline strongly suggests three probable causes:

**Normal discontinuities at mesh seams** are the most likely culprit. The NRD (NVIDIA Real-time Denoiser) used by Remix computes local curvature from per-pixel normals and uses **normal-based edge-stopping functions** that respect geometry boundaries. Where adjacent mesh pieces don't share vertex normals, the denoiser sees a sharp normal discontinuity and refuses to blend across it, leaving visible noisy or dark lines. NRD's documentation explicitly states: "Less accurate normals can lead to banding in curvature and local flatness."

**T-junctions** — where one triangle's edge meets the middle of another's — cause light leaking and shadow artifacts under path tracing that are invisible in rasterization. These manifest as bright or dark lines at mesh boundaries.

**Geometry merging issues** can occur when Remix auto-generates normals for meshes that don't provide them via FFP. If normals are generated per-submesh rather than across shared boundaries, discontinuities are guaranteed.

Fixes include: replacing meshes with properly welded geometry via USD replacements ensuring shared vertex normals at seams; adjusting denoiser parameters in Rendering → Denoising to loosen normal thresholds (at the cost of sharpness); enabling `rtx.enableBackfaceCulling` for secondary rays to reduce light bleeding at non-watertight seams; and using the Normals debug view to identify discontinuities.

---

## Fullscreen quads must never become world-space FFP geometry

RTX Remix detects screen-space elements primarily through **D3DFVF_XYZRHW / D3DDECLUSAGE_POSITIONT** vertex formats, which indicate pre-transformed screen coordinates. The texture categorization system then allows tagging specific draws by their texture hash.

If a fullscreen quad (post-processing bloom, HDR tonemapping, etc.) is incorrectly converted to FFP with world-space transforms, Remix will **interpret it as a scene object and attempt to path-trace it**. The Oblivion modding community documented this exact failure: a white quad mesh was drawn over the sky "in a very distracting and inconsistent manner," fixed by setting `bDoImageSpaceEffects=0`.

The recommended handling hierarchy:

- **Ignore entirely** (`rtx.ignoreTextures`): best for post-processing effects that Remix replaces with its own (bloom, tonemapping). The draw call is completely skipped.
- **Tag as UI** (`rtx.uiTextures`): Remix skips the draw during path tracing but composites it as a screen-space overlay on top of the final frame. Good for HUD elements.
- **Disable in the game**: configure the game to not issue the draw calls at all (INI settings, registry hacks, memory patches).
- **Pass through with POSITIONT**: if the proxy must forward the draw, use pre-transformed vertex format so Remix recognizes it as screen-space.

For TR Legend's proxy layer, fullscreen post-processing quads should be **detected and either skipped or tagged as ignored textures**, not converted to FFP world-space geometry.

---

## Culling removal requires game-side patching plus Remix anti-culling

Path tracing fundamentally requires geometry visible from all directions — not just the camera frustum — because shadow rays, reflection rays, and global illumination bounces travel in arbitrary directions. Game-side frustum culling, BSP/PVS culling, and occlusion culling must be addressed at two levels.

**Remix's built-in anti-culling system** (`rtx.antiCulling.object.*`) extends the lifetime of geometry that leaves the camera frustum by maintaining it in the ray-tracing acceleration structure. Key parameters include `fovScale` (expands the retention frustum FOV), `farPlaneScale` (extends the far plane, default **10×**), `numObjectsToKeep` (maximum retained instances, default **10,000**), and `enableInfinityFarPlane` (unbounded far plane). `enableHighPrecisionAntiCulling` uses Separating Axis Theorem for robust intersection checks, reducing flickering at the cost of some performance.

However, **Remix anti-culling alone is usually insufficient**. It can only retain objects that were drawn at least once — it cannot force the game to submit geometry it has already culled at the engine level. Community mods universally implement game-side culling removal through binary patching:

- **Source Engine**: `r_frustumcullworld 0` launch parameter plus binary patches to `c_frustumcull` (replace function with `xor al, al; retn` to always return "not culled") and `r_forcenovis` (disable BSP visibility)
- **Painkiller RTX**: replaced the entire portal/frustum system with **axis-aligned box culling** around the player — 6 planes defining a cube of configurable size. This yielded a **60→76 FPS improvement** and **82%→99% GPU utilization** compared to disabling all culling entirely
- **xoxor4d's mods**: implement "Remix-friendly culling" — everything within a configurable distance around the player renders, with per-map overrides and ImGui debug menus for real-time tweaking
- **UE2 games**: generic patcher (`ue2fixes`) can fix frustum and backface culling using signature scanning

The **box/sphere culling approach** is the proven best compromise: it prevents rendering the entire level (which causes CPU bottleneck from excessive draw calls) while ensuring all nearby geometry is submitted for path tracing. For TR Legend, this would require reverse-engineering the game's culling functions and either NOP-ing them out or replacing them with distance-based culling around the player camera position.

---

## Conclusion: a validated path forward for TR Legend

The technical approaches under consideration are largely validated by community practice, with important corrections. The D3D8 assumption is wrong — TR Legend is natively D3D9, simplifying the wrapper situation but not the core shader compatibility problem. The proven path is **FFP proxy conversion** following xoxor4d's framework pattern: hook the D3D9 device, intercept shader-based draw calls, resubmit geometry through the fixed-function pipeline with proper separated World/View/Projection matrices via `SetTransform`, set `fusedWorldViewMode = 0`, decode any SHORT4 vertices to float, tag fullscreen quads as ignored, and replace game-side frustum culling with distance-based box culling. Remix's anti-culling system supplements but does not replace game-side patching. Hash stability depends entirely on keeping the World matrix camera-independent. The vertex capture path exists as a fallback but remains experimental and fragile — the FFP conversion approach is what every successful shader-game Remix mod has used.