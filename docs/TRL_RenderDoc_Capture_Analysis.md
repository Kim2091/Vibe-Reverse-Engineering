# Tomb Raider Legend RenderDoc Capture Analysis

**Frame #1084 — Vulkan-level capture with FFP proxy + RTX Remix active**

---

## Capture Overview

These two files form a matched pair from a single captured frame of Tomb Raider Legend running through the full DLL chain (`trl.exe → dxwrapper → FFP proxy → RTX Remix → DXVK-Remix Vulkan`):

| File | Contents |
|------|----------|
| `1.txt` | RenderDoc event list — 5,888 lines, 3,265 Vulkan actions, every draw/dispatch/barrier in the frame |
| `111_chrome.json` | Chrome trace timing — 179,018 events with nanosecond timestamps for resource creation, draws, and pipeline compilation |

### Frame Statistics at a Glance

| Metric | Value |
|--------|-------|
| Total draw calls | 1,088 (1,087 indexed + 1 non-indexed) |
| Total colour passes | 427 |
| Total compute dispatches | 424 |
| Total triangles | ~42,695 |
| Vertex-captured draws | 663 (60.9%) |
| Non-captured draws | 425 (39.1%) |
| Graphics pipelines | 317 |
| Compute pipelines | 67 |
| Ray tracing pipelines | 2 |
| Shader modules | 696 |
| Buffers created | 10,799 |
| Images created | 438 |
| Command buffer submits | 7 |
| `vkCmdTraceRaysKHR` calls | **0** |
| `vkBuildAccelerationStructuresKHR` calls | **0** |

---

## The Big Picture: Remix Is Half-Working

RTX Remix initializes its ray tracing infrastructure but **never fires the path tracer**. The timing trace confirms 2 `vkCreateRayTracingPipelinesKHR` calls (~3.4 ms and ~3.6 ms each) — Remix fully compiles its RT pipelines during startup. But the frame contains **zero** acceleration structure builds and **zero** ray trace dispatches. The entire frame is purely rasterized through DXVK-Remix's fallback graphics pipeline.

---

## What IS Working: Vertex Capture

The vertex capture system is partially operational. Of 1,088 total draws, 663 (61%) have `Vertex Capture Buffer → Geometry Buffer` copies immediately following the draw call, meaning Remix is successfully reading back post-vertex-shader positions for those draws.

**Colour Pass #3 — the main scene geometry batch — has 100% vertex capture rate.** All 310 draws in this pass get both an `Index Cache Buffer` copy and a `Vertex Capture Buffer` copy. The proxy's FFP conversion is getting geometry data into Remix's pipeline for the primary scene pass.

| Draw Category | Count | Vertex Captured | Total Indices | Total Triangles |
|---------------|-------|-----------------|---------------|-----------------|
| Captured draws | 663 | YES | 88,893 | ~29,631 |
| Non-captured draws | 425 | NO | 39,192 | ~13,064 |
| **Total** | **1,088** | — | **128,085** | **~42,695** |

---

## What ISN'T Working: The Largest Draws Are All Uncaptured

The 9 largest draws in the frame are **all** in the non-captured category:

| Index Count | Triangles | Vertex Captured | Likely Content |
|-------------|-----------|-----------------|----------------|
| 4,668 | 1,556 | NO | Environment (shadow/lighting pass) |
| 3,852 | 1,284 | NO | Environment (shadow/lighting pass) |
| 3,600 | 1,200 | NO | Environment (shadow/lighting pass) |
| 2,682 | 894 | NO | Environment (shadow/lighting pass) |
| 2,340 | 780 | NO | Environment (shadow/lighting pass) |
| 1,890 | 630 | NO | Environment (shadow/lighting pass) |
| 1,824 ×3 | 608 ×3 | NO | Lara/NPC model (3 shadow cascades) |
| 1,062 | 354 | YES | *(first captured entry in top 30)* |

The pattern of 3× repetition at the same index count (1,824, 744, 936) strongly suggests **multi-pass rendering** — the same mesh drawn once per shadow cascade or lighting pass, which is exactly what TRL's "Next Generation Content" mode does.

---

## Frame Structure: Five Distinct Rendering Phases

### Phase 1 — Main Scene Geometry (Passes #1–3, actions 1–945)

The bulk of the frame. Pass #3 alone spans 930 actions and contains 310 draws — all vertex-captured, all with the repeating pattern:

```
BeginRenderPass → DrawIndexed → EndRenderPass → CopyBuffer(Index Cache) → CopyBuffer(Vertex Capture)
```

This is the primary world geometry and character rendering. Every draw gets its own render pass (no batching within passes), which is characteristic of DXVK-Remix wrapping each D3D9 draw call individually. The proxy's FFP conversion handles this entire phase — decomposing WVP matrices, feeding separated W/V/P transforms to Remix, cleaning texture stages to albedo-only.

**Draw size distribution in this phase:**

| Index Range | Draw Count | Description |
|-------------|------------|-------------|
| 1–6 | ~74 | Tiny patches, decals |
| 7–24 | ~122 | Small geometry elements |
| 25–100 | ~137 | Medium world chunks |
| 101–500 | ~313 | Standard world geometry |
| 500+ | ~16 | Large environment pieces |

### Phase 2 — Shadow Maps / Multi-Pass Lighting (Passes #4–25, actions 946–1014)

A series of **single-draw passes with large index counts**, each followed by a compute dispatch. No vertex capture on the large draws. This is almost certainly TRL's NGC shadow map and auxiliary lighting rendering — the game re-drawing scene geometry from light-space viewpoints.

```
Pass #4:  DrawIndexed(33) [captured] + DrawIndexed(471) [NOT captured] → Dispatch(5,1,1)
Pass #5:  DrawIndexed(486)  [NOT captured] → Dispatch(5,1,1)
Pass #6:  DrawIndexed(330)  [NOT captured] → Dispatch(1,1,1)
Pass #7:  DrawIndexed(1824) [NOT captured] → Dispatch(3,1,1)
Pass #8:  DrawIndexed(1824) [NOT captured] → Dispatch(3,1,1)
Pass #9:  DrawIndexed(1824) [NOT captured] → Dispatch(3,1,1)
  ...
Pass #25: DrawIndexed(4668) [NOT captured] → Dispatch(58,1,1)
```

The proxy likely routes these as shader passthrough because the WVP matrix or vertex declaration doesn't match FFP criteria. The compute dispatch sizes scale with vertex count (dispatch(58,1,1) follows the 4,668-index draw), confirming these are Remix's per-draw vertex capture compute shaders — they execute but the capture buffer copy doesn't happen, suggesting Remix rejects the geometry at the capture validation stage.

### Phase 3 — Secondary World Geometry (Passes #26, #29, actions 1015–1461)

Two large multi-draw passes with vertex capture active:

- **Pass #26** (137 actions): 45 draws, varied sizes (3–720 indices), all captured. Additional world geometry in a second render pass.
- **Pass #29** (303 actions): ~100 draws of mostly tiny geometry (3–33 indices), captured. Contains the frame's only `vkCmdBlitImage` (resolve/downsample). This looks like TRL's decal or detail geometry pass.

### Phase 4 — Post-Processing / Effects (Passes #30–401, actions 1462–2920)

Hundreds of single-draw passes, mostly with tiny draw calls (3–6 indices = single fullscreen triangles/quads). The 389 draws with 1–6 indices that lack vertex capture are concentrated here. This is TRL's post-processing chain — bloom, HDR tonemapping, color grading — rendered as fullscreen quads that correctly should not enter the path tracing pipeline.

### Phase 5 — Final Compositing (Passes #402–427, actions 2921–3264)

The frame's tail end. Pass #402 has 221 actions with vertex-captured draws (late-rendered world geometry or UI elements). Passes #411–425 each contain a single draw followed by a dispatch. The frame ends with a `vkCmdDraw(3,1)` (final fullscreen triangle blit) and `vkQueuePresentKHR`.

---

## Non-Captured Draw Analysis

### Index Count Distribution

| Index Range | Non-Captured Count | Interpretation |
|-------------|-------------------|----------------|
| 1–6 | 389 | Fullscreen quads / post-process (correct to skip) |
| 7–24 | 1 | Likely small effect geometry |
| 25–100 | 7 | Possibly particle effects or small decals |
| 101–500 | 7 | Medium geometry in shadow/lighting passes |
| 500+ | 21 | **Large scene geometry in shadow passes — the problem** |

The 389 tiny non-captured draws are expected and correct — they're post-processing passes. The 21 large non-captured draws (500+ indices, totaling ~13,000 triangles) represent the multi-pass shadow/lighting geometry that the proxy isn't handling for FFP conversion.

---

## Compute Dispatch Pattern

All 424 dispatches are Remix's per-draw vertex capture compute shaders:

| Dispatch Size | Count | Interpretation |
|---------------|-------|----------------|
| (1, 1, 1) | 398 | Small geometry (< ~64 vertices per workgroup) |
| (3, 1, 1) | 9 | Medium geometry (~128–192 vertices) |
| (5, 1, 1) | 2 | ~256–320 vertices |
| (8, 1, 1) | 3 | ~512 vertices |
| (17–58, 1, 1) | 12 | Large geometry (1,000–4,000+ vertices) |

The dispatch count (424) doesn't match the draw count (1,088) because not every draw triggers a compute dispatch — only those where Remix attempts vertex capture. The compute shaders execute for 424 draws, but only 663 get the subsequent `CopyBuffer(Vertex Capture)`, meaning some dispatches run but their results aren't committed to geometry buffers.

---

## Ray Tracing Pipeline — Created But Never Used

The Chrome trace confirms full RT pipeline setup:

| Resource Type | Count | Notes |
|---------------|-------|-------|
| `vkCreateRayTracingPipelinesKHR` | 2 | ~3.4 ms and ~3.6 ms each |
| `vkCreateComputePipelines` | 67 | Includes RTXDI, ReSTIR GI, NRD stages |
| `vkCreateGraphicsPipelines` | 317 | Per-material rasterization fallbacks |
| `vkCreateShaderModule` | 696 | All shader stages compiled |

Remix has everything it needs to path-trace **except a validated scene**. The path from vertex capture to acceleration structure build requires:

1. ✅ Vertex capture compute shaders execute
2. ✅ Index data copied to cache buffers
3. ⚠️ Vertex capture data copied to geometry buffers (only 61% of draws)
4. ❌ Camera validation (View + Projection matrices recognized as valid)
5. ❌ BLAS build per unique geometry hash
6. ❌ TLAS assembly for the frame
7. ❌ RTXDI / ReSTIR GI / NRD / DLSS pipeline launch

---

## Why the Path Tracer Doesn't Fire

The most likely cause is **camera validation failure**. Remix's scene manager needs valid View and Projection matrices via `SetTransform` to establish a camera. Even though vertex data is being captured, if Remix doesn't see a valid camera, it never builds acceleration structures or launches the RT pipeline.

### Possible Failure Points

| Cause | Likelihood | Evidence |
|-------|------------|----------|
| `SetTransform(VIEW/PROJ)` not reaching Remix | **High** | The dxwrapper `SetTransform` conflict (§10 in pipeline doc) could swallow proxy transforms between draws |
| Matrix values failing validation | Medium | Near-identity, NaN, or malformed matrices from decomposition edge cases |
| Timing mismatch | Medium | Transforms set after first draw, missing Remix's per-frame camera extraction window |
| `rtx.fusedWorldViewMode` mismatch | Low | Should be 0 for separated W/V/P — verify in rtx.conf |
| Insufficient valid geometry | Low | 663 captured draws should be more than enough |

---

## Actionable Takeaways

### 1. Debug the Camera First

The camera is the gating issue. Cross-reference `ffp_proxy.log` output for frames captured around the same time:

- Verify `SetTransform(D3DTS_VIEW)` and `SetTransform(D3DTS_PROJECTION)` are called with non-identity, non-zero matrices **before** the first draw
- If they are reaching Remix: check `rtx.fusedWorldViewMode=0` in `rtx.conf`
- If they aren't reaching Remix: the dxwrapper `SetTransform` conflict intercept needs hardening — ensure the proxy's own `SetTransform` calls bypass the intercept and hit the real vtable directly

### 2. The Vertex Capture Path Is the Strongest Signal of Progress

310 draws in the main pass are fully captured with both index and vertex data. The proxy's draw routing for world geometry is working. Once the camera validates, these draws should produce hashable geometry for the scene manager.

### 3. Handle the Shadow/Multi-Pass Region (Passes #4–25)

The 1,824-index repeated draws are almost certainly Lara rendered from shadow-light viewpoints. Options:

- **Skip them** (return 0 from `DrawIndexedPrimitive`) — they don't contribute to the primary path-traced scene
- **Tag their textures** in `rtx.ignoreTextures` so Remix doesn't try to process them
- **Detect them** by checking for render target changes or VP matrix changes mid-frame that indicate shadow pass rendering

### 4. The 389 Tiny Non-Captured Draws Are Correct

These are post-processing fullscreen quads (1–6 indices). The proxy's screen-space quad detection is working — these correctly bypass vertex capture and won't pollute the path-traced scene.

### 5. The Compute Dispatches Confirm Remix's Internals Are Functional

The 424 compute dispatches are Remix's vertex capture compute shaders operating correctly. The 398 `(1,1,1)` dispatches handle small draws while larger dispatches like `(58,1,1)` scale with vertex count. The internal pipeline is live — it just needs a valid camera to proceed to acceleration structure building.

---

## Appendix: Resource Creation Timing

From the Chrome trace (`111_chrome.json`):

| Operation | Count | Total Time | Avg Per Call |
|-----------|-------|------------|-------------|
| RT pipeline creation | 2 | 6.96 ms | 3.48 ms |
| Shader module creation | 696 | 1.53 ms | 2.2 µs |
| Graphics pipeline creation | 317 | 0.79 ms | 2.5 µs |
| Compute pipeline creation | 67 | varies | up to 4.2 ms |
| Buffer creation | 10,799 | — | — |
| Image creation | 438 | — | — |
| ImageView creation | 1,665 | — | — |
| Descriptor set allocation | 23,953 | — | — |
| Debug name assignment | 35,133 | — | — |
