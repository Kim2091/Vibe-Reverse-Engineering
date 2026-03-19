# Tomb Raider Legend RTX Remix / FFP Handoff

This document is the current source of truth for the Tomb Raider Legend fixed-function / RTX Remix effort in this repository. It is written for both humans and future LLM sessions.

It covers:

- what the game and runtime stack actually are
- which repo folders matter
- what was tried, in roughly chronological order
- what definitely worked
- what definitely failed
- what the competing proxy branches mean
- what the next LLM should do instead of re-discovering the same dead ends

## Executive Summary

`trl.exe` is not a native D3D9 title. It is a D3D8 game that is being converted to D3D9 via `dxwrapper.dll`, and only then intercepted by the FFP proxy / Remix chain. That detail matters because it explains why some assumptions from stock D3D9 workflows did not hold.

The most important confirmed fact so far is this:

- The proxy and the Remix chain can coexist without breaking the game if the proxy is in passthrough mode.
- The failure is specifically in the FFP conversion logic, not in basic DLL injection or chain loading.
- There are two competing transform models in the repo:
  - `patches/trl_legend_ffp`: older, more advanced, assumes separate `WVP`, `World`, `View`, and `ViewProjection`-style constants and adds culling/frustum/hash-stability work.
  - `patches/TombRaiderLegend`: later experimental branch that treats `c0-c3` as a fused per-draw WVP and pushes identity `View`/`Projection`.
- Neither model has yet produced a stable, correct Remix-rendered 3D scene.

If you are a future LLM, do not restart from the stock template. Start from the repo state summarized here.

## Repo Index

These are the files and folders that matter most.

| Path | Purpose | Notes |
| --- | --- | --- |
| `rtx_remix_tools/dx/dx9_ffp_template/` | Stock DX9 shader-to-FFP proxy template | Baseline, not TRL-specific |
| `patches/trl_legend/` | Early TRL-specific helper scripts and traces | Contains culling patch, registry helper, live trace JSONLs |
| `patches/trl_legend_ffp/` | Advanced TRL-specific FFP proxy branch | Most feature-rich committed branch |
| `patches/TombRaiderLegend/` | Later experimental TRL proxy branch | Fused-WVP experiment |
| `ffp_proxy.log` | Large diagnostic proxy log captured during prior runs | Useful, but easy to over-interpret |
| `vs_constants.txt` | Static analysis summary of VS constant call sites | Good quick reference |
| `checkpoint.py` | Repo-local checkpoint workflow | Use before risky experimentation |
| `.cursor/rules/git-checkpoints.mdc` | Always-on checkpoint rule | Tells future agents to checkpoint before risky edits |

Important transcript references:

- [Checkpoint Setup](1a81e33f-bdbe-4175-9d44-19f86ad68135)
- [Initial Static Pass](1a4885ea-7129-4030-ac72-256e76045318)
- [First FFP Attempt](e87a3a88-77cb-4d75-b328-327020623bab)
- [Failure Review](09830a24-5803-4040-b482-8ce26e33c64a)
- [Backup Runtime Debug](7aac336f-56af-472b-b79a-53600810ea21)

## Runtime Stack

The effective graphics stack for TRL is:

1. `trl.exe`
2. `dxwrapper.dll`
3. D3D8-to-D3D9 conversion
4. `d3d9.dll` proxy
5. Remix bridge client
6. `.trex/NvRemixBridge.exe`
7. `.trex/d3d9.dll` DXVK-Remix runtime

Confirmed from `dxwrapper.ini`:

- `D3d8to9 = 1`

That means every proxy assumption must be filtered through the fact that TRL is effectively a D3D8 renderer translated into D3D9 state.

## Current Game-Side Config Snapshot

Current `proxy.ini` in the game directory:

- `Enabled=1`
- `DLLName=d3d9.dll.bak`
- `AlbedoStage=0`
- `DisableNormalMaps=0`
- `ForceFfpSkinned=0`
- `ForceFfpNoTexcoord=0`
- `FrustumPatch=0`

This is more conservative than the older advanced proxy config committed in `patches/trl_legend_ffp/proxy/proxy.ini`, which enables:

- `DisableNormalMaps=1`
- `ForceFfpNoTexcoord=1`
- `FrustumPatch=1`
- `DLLName=d3d9.dll.bak`

Current Remix classification / compatibility config in `user.conf` includes:

- `rtx.useVertexCapture = True`
- `rtx.fusedWorldViewMode = 1`
- `rtx.zUp = True`
- `rtx.orthographicIsUI = True`
- large curated hash lists for:
  - `rtx.worldSpaceUiBackgroundTextures`
  - `rtx.worldSpaceUiTextures`
  - `rtx.smoothNormalsTextures`
  - `rtx.ignoreTextures`
  - `rtx.skyBoxTextures`
  - `rtx.uiTextures`
  - `rtx.decalTextures`
  - `rtx.hideInstanceTextures`
  - `rtx.animatedWaterTextures`
  - `rtx.raytracedRenderTargetTextures`

This is the main evidence that texture-hash stabilization work happened at the runtime-config layer, not only in proxy code.

## Static Analysis Discoveries

`vs_constants.txt` established four direct `SetVertexShaderConstantF` call sites:

- `0x00ECBA57`
- `0x00ECBB89`
- `0x00ECBC01`
- `0x00ECC3C4`

It also identified many indirect `+0x178` accesses, plus multiple `DrawIndexedPrimitive` and `SetVertexDeclaration` call sites.

The early static/decompiler work concluded that the renderer definitely uses:

- `SetVertexShaderConstantF`
- `SetVertexShader`
- `SetVertexDeclaration`
- `DrawIndexedPrimitive`
- D3DX constant-table style bindings

This made TRL look like a plausible candidate for the DX9 FFP proxy workflow.

## Live Trace and Log Discoveries

The strongest runtime artifacts currently committed are:

- `patches/trl_legend/trace_vsconst_hist.jsonl`
- `patches/trl_legend/trace_reg0.jsonl`
- `ffp_proxy.log`

### Confirmed VS Constant Patterns

Across runs, the most repeated patterns were:

- `start=0, count=4`
- `start=6, count=1`
- `start=28, count=1`
- less frequent `start=8, count=8`

`trace_vsconst_hist.jsonl` is the cleanest proof of that distribution.

### Reg0 Trace Evidence

`trace_reg0.jsonl` showed that `start=0, count=4` was not a single simple constant:

- Sometimes it looked like a projection-style matrix:
  - `2, 0, 0, 0`
  - `0, -2.285714, 0, 0`
  - `0, 0, 1.000122, -16.001953`
  - `0, 0, 1, 0`
- Other times it contained large translations:
  - examples include values like `278.804688`, `-24795.994141`, `9956.574219`, `-8269.916016`, `-7212.641602`, `-14924.438477`

This is why the later investigation started treating `c0-c3` as a fused or overloaded transform slot rather than a pure projection register.

### Early Log Interpretation

The large `ffp_proxy.log` in the repo initially led to this model:

- `c0-c3` looked projection-like
- `c8-c15` looked like a once-per-frame matrix block
- `c6` and `c28` looked like per-object scalar / offset data

That interpretation was useful, but incomplete.

### Later Revised Interpretation

The later passthrough diagnostic pass revised the model:

- `c0-c3` changes much more often than originally assumed
- not every `c0` write is the same kind of matrix
- there appear to be at least two families of uses:
  - screen-space / UI / post-process-like
  - real 3D world geometry

This is the key reason the project diverged into two proxy branches.

## The Three Major Implementation Phases

## 1. Early TRL Helper Phase

This phase is represented mostly by `patches/trl_legend/`.

Important files:

- `patches/trl_legend/disable_culling.py`
- `patches/trl_legend/set_remix_compat.py`
- `patches/trl_legend/trace_vsconst_hist.jsonl`
- `patches/trl_legend/trace_reg0.jsonl`

### What This Phase Added

- A direct EXE patch to force `D3DCULL_NONE`
- A registry helper for graphics settings more compatible with Remix testing
- Live trace captures that proved the actual VS constant traffic was more complex than the stock template expected

### EXE Culling Patch

`disable_culling.py` patches `trl.exe` at:

- `VA 0x40EEA7`

It replaces logic that built `D3DRS_CULLMODE` from packed render-state bits with a constant `D3DCULL_NONE`.

This patch:

- creates a `.bak` of `trl.exe`
- can verify current state
- can restore the original bytes

### Graphics Registry Helper

`set_remix_compat.py` targets:

- `HKCU\Software\Crystal Dynamics\Tomb Raider: Legend\Graphics`

Recommended values written by the helper:

- `UseShader00 = 0`
- `UseShader10 = 0`
- `UseShader20 = 0`
- `UseShader30 = 1`
- `DisablePureDevice = 1`
- `DisableHardwareVP = 0`
- `UseRefDevice = 0`

The committed `graphics_registry_backup.json` shows one saved state where:

- `UseShader30 = 1`
- `DisablePureDevice = 0`

So this was an area of experimentation, not a permanently settled answer.

## 2. Advanced CTAB / Decomposition Proxy

This phase is committed as `patches/trl_legend_ffp/`.

This is the most feature-rich and most deliberate TRL-specific proxy branch in the repo.

### Transform Model in Later Branch

At the top of `patches/trl_legend_ffp/proxy/d3d9_device.c`, the committed assumptions are:

- `WorldViewProject` at `c0`
- `World` at `c4`
- `View` at `c8`
- `ViewProject` at `c12`
- `CameraPos` at `c16`
- `TextureScroll` at `c26`
- utility constant at `c39`
- `SkinMatrices` beginning at `c48`

The FFP transform strategy in this branch is:

- read `WVP` from `c0`
- read `World` from `c4`
- read `View` from `c8`
- derive `Projection` by:
  - `worldInv = inverse(World)`
  - `viewInv = inverse(View)`
  - `proj = viewInv * (worldInv * WVP)`
- if that cannot be done cleanly, fall back to:
  - `View = identity`
  - `Projection = WVP`
  - `World = identity`

This is a much more sophisticated branch than the stock template.

### FFP Routing Rules in This Branch

In `WD_DrawIndexedPrimitive`, the proxy only converts draws to FFP when all of the following are true:

- `viewProjValid` or `wvpValid`
- primitive type is triangle list (`pt == 4`)
- `streamStride[0] >= 12`
- declaration has texcoords, or `ForceFfpNoTexcoord` is enabled

Skinned draws can:

- pass through
- or be forced through FFP when `ForceFfpSkinned` is enabled and a bone palette exists

### Hash Stability and Texture Stability Work

This branch has the clearest committed anti-instability logic.

It does all of the following:

- forces stage 0 to a deterministic color path
- disables stages 1-7 during FFP
- optionally strips non-albedo texture stages via `DisableNormalMaps`
- blocks `SetTextureStageState` while FFP is active
- blocks `D3DSAMP_MIPMAPLODBIAS` changes in `WD_SetSamplerState`
- forces `D3DRS_CULLMODE = D3DCULL_NONE`
- disables fog and clip planes in FFP setup

The comments explicitly say some of this is for:

- stable Remix hashes
- sharper textures
- preventing texture blur during camera motion

### Frustum / Culling Work

This branch also contains the strongest committed CPU-culling intervention.

There are two separate mechanisms:

1. Continuous frustum threshold patch
2. Frustum matrix widening

Important addresses:

- `FRUSTUM_THRESHOLD_ADDR = 0x00EFDD64`
- global frustum / VP matrix at `0x00F3C5C0`

Important behavior:

- `BeginScene` writes `-FLT_MAX` to `0x00EFDD64` so projected-Z comparisons pass
- `Present` and `FFP_WidenFrustum()` clamp the XY rows of the matrix at `0x00F3C5C0` to widen visibility
- backface culling is globally suppressed in `WD_SetRenderState`
- clip planes are disabled in FFP setup

This means culling was attacked at multiple layers:

- CPU-side frustum threshold
- CPU-side frustum matrix shape
- D3D render-state cull mode
- clip-plane state

## 3. Later Fused-WVP Experimental Proxy

This phase is committed as `patches/TombRaiderLegend/`.

It is the clearest representation of the later chat theory that:

- `c0-c3` is the only transform that really matters for 3D object positioning
- `c0-c3` should be treated as fused `WorldViewProjection`
- `View` and `Projection` should be identity
- Remix should decompose the fused transform

### Transform Model Used Here

The top comments in this branch say:

- `c0-c3`: combined WVP, updated per draw call
- `c6`: per-object scalar
- `c8-c15`: base matrices, written once per frame
- `c28`: world-space offset

The actual FFP logic:

- writes `c0-c3` into `D3DTS_WORLD`
- writes identity into `D3DTS_VIEW`
- writes identity into `D3DTS_PROJECTION`
- marks the transform valid as soon as `c0-c3` is written once

This branch is much simpler than `trl_legend_ffp`, but also much riskier because it depends on Remix accepting that transform model.

### Why This Branch Was Tried

It came from a later diagnostic realization:

- the game rendered normally through the proxy in pure passthrough mode
- the proxy chain itself was therefore not the root problem
- later logs suggested `c0-c3` changed far more often than once per frame
- this looked like evidence of a per-draw combined transform rather than a static projection matrix

### Outcome

This branch did not solve the problem.

Observed result from the chats:

- Remix hooked
- rendering stayed broken
- geometry still did not appear correctly
- experiments with `rtx.fusedWorldViewMode` values did not produce a stable success

## What Definitely Worked

These are confirmed, not hypothetical.

### Working Result 1: Baseline Remix Without Proxy

When the FFP proxy was bypassed and the Remix bridge client was used directly as `d3d9.dll`:

- the game rendered normally
- Remix hooked
- Remix still did not render meaningful geometry

Interpretation:

- the runtime stack itself can launch
- TRL's original shader path is not directly usable by Remix

### Working Result 2: Passthrough Proxy + Remix

When the proxy was compiled into a pure passthrough mode:

- the game rendered normally
- Remix hooked
- the chain `trl.exe -> dxwrapper -> proxy -> Remix bridge` was proven sound

Interpretation:

- the wrapper and chain-loading architecture are not the problem by themselves
- the problem starts when actual FFP conversion is enabled

### Working Result 3: Diagnostic Capture

The tooling successfully produced:

- static call-site inventories
- large proxy logs
- focused live JSONL traces
- enough runtime evidence to disprove several wrong transform assumptions

## What Definitely Failed

These are also confirmed.

### Failure 1: Naive Separate Matrix Mapping

The early model:

- `c0-c3 = projection`
- `c8-c11 = view`
- `c12-c15 = world`

did not produce correct FFP rendering.

Observed outcomes:

- black screen
- or Remix hook with empty / blue scene

### Failure 2: Identity World + Always Apply

A later attempt assumed:

- per-object world transforms were effectively baked already
- the proxy should always apply transforms
- `World = identity`

This also failed to restore stable geometry.

### Failure 3: Fused-WVP as World + Identity View/Projection

The later `patches/TombRaiderLegend` strategy, combined with `rtx.fusedWorldViewMode` experimentation, still did not produce a working 3D render in Remix.

### Failure 4: Direct `.trex\\d3d9.dll` Chain-Load Experiment

One chat experimented with modifying chain loading so the proxy would directly load `.trex\\d3d9.dll`.

That did not become the committed or stable solution.

The working chain-load convention that kept returning was:

- `DLLName=d3d9.dll.bak`

or equivalent root-level bridge-client naming.

### Failure 5: Assuming One Log Explained Everything

One of the biggest process failures was over-trusting a single proxy log interpretation.

Specifically:

- one phase concluded `c0-c3` was basically projection-only
- a later phase proved `c0-c3` can also carry translated per-draw matrices

So any future work must re-validate transform assumptions with live traces, not only with stale logs.

## Backup and Runtime Findings

The runtime backups mattered because they preserved a more advanced proxy configuration than the stock template.

The key backup from the chats was:

- `ffp-backup-20260314-222624`

The important facts recovered from chat history and committed repo state are:

- it used a 16 KB FFP proxy
- it chain-loaded `d3d9.dll.bak`
- it enabled advanced knobs:
  - `DisableNormalMaps`
  - `ForceFfpSkinned`
  - `ForceFfpNoTexcoord`
  - `FrustumPatch`

One subtle but important point:

- At one point the chats believed the source for that backup DLL was missing.
- In the current repo, `patches/trl_legend_ffp/` appears to be that advanced line or something very close to it.

So a future LLM should treat `patches/trl_legend_ffp/` as the best committed approximation of the advanced backup, not assume the backup is source-less.

## Hash Stability Work: What Exists and What Does Not

The user asked specifically about making hashes stable.

What does exist:

- `user.conf` contains extensive texture hash classification for TRL
- `trl_legend_ffp` freezes many state changes that would destabilize Remix capture:
  - texture stages
  - non-albedo stages
  - mip LOD bias
  - culling and clipping
- comments in `trl_legend_ffp` explicitly mention stable Remix hashes

What I did not find in the committed repo or transcripts:

- a standalone algorithmic patch that changes Remix's hashing itself
- a dedicated proxy-side "stable hash generator"

So the current state of "hash stabilization" is:

- curated runtime-side texture classification
- deterministic FFP texture/sampler/render-state setup
- suppression of game-side state churn that made hashes unstable

## Best Current Interpretation

The strongest evidence points to this:

- TRL does not cleanly expose a single, stable `World` / `View` / `Projection` triple in the way the stock template wants
- different shader families likely use different matrix conventions
- `c0-c3` is overloaded enough that treating it as always-projection or always-WVP is too simplistic
- the advanced `trl_legend_ffp` branch is likely closer to the truth than the later fused-WVP branch, because it acknowledges:
  - a dedicated `WVP`
  - a separate world path
  - a separate `ViewProjection` path
  - skinning, no-texcoord forcing, and culling/frustum side effects

That does not mean `trl_legend_ffp` is solved. It means it is the more complete starting point.

## Recommended Starting Point for the Next LLM

If a future LLM continues this work, it should do the following in order.

1. Start from `patches/trl_legend_ffp`, not from `rtx_remix_tools/dx/dx9_ffp_template`.

2. Diff `patches/trl_legend_ffp/proxy/d3d9_device.c` against `patches/TombRaiderLegend/proxy/d3d9_device.c` and isolate only the transform-model differences.

3. Reproduce the known-good baseline first:
   - proxy in passthrough mode
   - game renders normally
   - Remix hooks

4. Confirm the exact current game-side runtime stack before any edits:
   - `dxwrapper.dll`
   - `dxwrapper.ini`
   - `d3d9.dll`
   - `d3d9.dll.bak`
   - `.trex/`
   - `proxy.ini`
   - `user.conf`
   - `rtx.conf`
   - `dxvk.conf`

5. Re-run live tracing in an actual 3D level instead of relying only on archived logs.

6. Specifically trace the call path corresponding to the `00ECBA5D` caller seen in `trace_reg0.jsonl` and `trace_vsconst_hist.jsonl`, and capture full float payloads for:
   - `start=0, count=4`
   - `start=6, count=1`
   - `start=28, count=1`
   - `start=8, count=8`

7. Re-validate whether `c4-c7`, `c8-c11`, and `c12-c15` are really populated in live geometry draws on the current runtime.

8. Keep culling/frustum work and transform work separated while debugging:
   - first prove correct transform mapping
   - then decide which culling/frustum relaxations are actually needed

9. Preserve the texture-hash classification work already present in `user.conf`; do not throw it away while debugging transforms.

10. Use `checkpoint.py` before each major proxy edit or deployment experiment.

## Suggested Concrete Next Experiments

These are the most efficient next experiments based on what is already known.

### Experiment A: Rebase on `trl_legend_ffp`

Treat `trl_legend_ffp` as the mainline.

Goal:

- keep advanced stability features
- keep culling/frustum patching
- test whether the matrix decomposition branch can be repaired instead of replaced

### Experiment B: Runtime Truth Table

Build a tiny diagnostic branch that logs, for the first few geometry draws only:

- current VS handle
- current declaration pointer
- `c0-c3`
- `c4-c7`
- `c8-c11`
- `c12-c15`
- whether `curDeclHasTexcoord`
- whether the draw is skinned

Goal:

- prove which matrix ranges are actually valid for world geometry
- stop inferring from mixed UI / post-process / geometry traffic

### Experiment C: Preserve Stability, Swap Transform Core

Keep these from `trl_legend_ffp`:

- cull suppression
- clip/fog suppression
- stable texture stage setup
- LOD bias blocking
- hash-friendly config

Swap only:

- the transform extraction and application logic

Goal:

- avoid losing the stability work while iterating on transforms

## LLM Warning List

Future LLMs should avoid these mistakes.

- Do not assume TRL is a D3D9-native game. It is D3D8 through `dxwrapper`.
- Do not assume the stock template mapping applies.
- Do not assume `c0-c3` is always projection.
- Do not assume `c0-c3` is always WVP either.
- Do not treat one archived `ffp_proxy.log` as ground truth without live confirmation.
- Do not throw away the `user.conf` hash lists.
- Do not debug culling and transforms in the same step unless the transform model is already confirmed.
- Do not restart from the generic template unless you intentionally want to discard all TRL-specific knowledge.

## Operational Notes

The repo now includes a working checkpoint system:

- `checkpoint.py`
- `.cursor/rules/git-checkpoints.mdc`

Use it like this:

```bash
python checkpoint.py save "before-trl-transform-rework"
python checkpoint.py list
python checkpoint.py restore "before-trl-transform-rework"
```

The git remote setup from the support chat is also already configured:

- `origin` -> your fork
- `upstream` -> Kim's repo
- `ekozmaster` -> original fork/source repo

This means future work can be checkpointed locally and pushed to your own fork without redoing repo setup.

## Bottom Line

The project is not stuck because injection failed. Injection and passthrough are proven.

The project is stuck because TRL's transform convention, draw-family split, and D3D8-to-D3D9 translation do not fit the stock FFP template cleanly. The repo already contains two serious attempts to solve that:

- `trl_legend_ffp`: decomposition-heavy, stability-heavy, culling/frustum-aware
- `TombRaiderLegend`: fused-WVP experimental rewrite

Neither is final, but together they encode most of the expensive discovery work. Any next session should begin by preserving those assets, validating them against live traces, and only then changing the transform model.

## 4. Session 2026-03-17: Frame Trace Analysis + Proxy Hardening

This session used the DX9 frame tracer (`dxtrace_frame.jsonl`, 85MB, 149252 records) to establish ground truth about TRL's rendering pipeline. All findings below are from live frame capture data, not inference.

### Ground Truth: Shader Constant Layout (CONFIRMED via CTAB)

Every vertex shader in TRL uses D3DX9 Shader Compiler 9.11.519 with these constant tables:

| Name | Register | Size | Notes |
|------|----------|------|-------|
| `WorldViewProject` | c0 | 4 regs | **Only transform matrix in ANY shader** |
| `fogConsts` | c4 | 1 reg | Fog parameters |
| `textureScroll` | c6 | 1 reg | UV animation (mad oT0.xy, v2, c6.w, c6) |
| `bendConstants` | c8 | 8 regs | Vegetation bending (some shaders) |
| `lightInfo` | c16+ | variable | Per-vertex lighting (some shaders) |
| `envMatrix` | c40 | 2 regs | Environment mapping (some shaders) |

**Critical fact**: There is NO separate World, View, or Projection matrix in any shader. c4-c7 and c8-c15 are NOT matrices — they are fog, scroll, and bend parameters. Previous assumptions that c4=World, c8=View, c12=ViewProj were wrong.

### Ground Truth: SetVertexShaderConstantF Traffic

From frame trace `--const-provenance`:

| Pattern | Frequency | Content |
|---------|-----------|---------|
| `start=0, count=4` | Per-draw | WVP matrix (changes every object) |
| `start=8, count=8` | Once per frame | Small values (0.01 range) — frustum/bend vectors, NOT matrices |
| `start=6, count=1` | Per-draw | Texture scroll params |
| `start=28, count=1` | Per-draw | World-space offset |

Confirmed: c8-c15 values at draw time are `[-0.0064, 0.0329, 0, 0]` etc. — these are NOT camera/view matrices.

### Ground Truth: Vertex Declarations

| Declaration | Stride | Elements | Used for |
|------------|--------|----------|----------|
| SHORT4 + D3DCOLOR + SHORT2 + SHORT2 | 20 bytes | POSITION SHORT4, COLOR, TEXCOORD0, TEXCOORD1 | World geometry (~90% of draws) |
| FLOAT3 + D3DCOLOR + FLOAT2 | 24 bytes | POSITION FLOAT3, COLOR, TEXCOORD0 | Overlays, some character draws |

Neither declaration has NORMAL elements. Remix must generate normals from face geometry.

### Ground Truth: Draw Statistics (per frame)

- 3187 total draws, all DrawIndexedPrimitive
- 1380 fullscreen quads (43% of draws)
- 548 opaque draws
- 2636 alpha-tested
- 1778 alpha-blended (SRCALPHA, INVSRCALPHA)
- 1296 SetTransform calls (from dxwrapper D3D8→D3D9 conversion)
- 3187 SetMaterial calls
- ~70K SetTextureStageState calls
- ~19K SetRenderState calls
- 6 unique vertex shaders total

### Ground Truth: Game Memory Matrices

The proxy reads View and Projection from fixed game memory addresses. These ARE valid:

- `TRL_VIEW_ADDR = 0x010FC780` — contains a real camera View matrix (rotation + large translation like 11525, 58449, -21945)
- `TRL_PROJ_ADDR = 0x01002530` — contains a standard D3D perspective projection: `[2.0, 0, 0, 0; 0, -2.28, 0, 0; 0, 0, 1.0, 1.0; 0, 0, -16.0, 0]`

The game-memory decomposition `World = WVP * (View * Proj)^-1` produces correct near-identity rotation with world-space translation for per-object World matrices.

### Ground Truth: dxwrapper SetTransform Patterns

dxwrapper's D3D8→D3D9 conversion calls SetTransform with these patterns:

- **3D geometry**: View=identity, Proj=identity, World=WVP-combined
- **Overlay draws**: View=identity, Proj=real projection, World=identity

dxwrapper NEVER provides a real View matrix via SetTransform. It always uses identity.

### What Worked This Session

#### Working Result 4: Lara with RTX Path Tracing

**Build**: Game-memory decomposition + intercepted SetTransform/SetRenderState/SetMaterial/SetTextureStageState + per-draw wvpDirty=1 + per-draw FFP_SetupLighting + fallbackLightMode=1

**What was visible**: Lara's character model rendered with proper path-traced rim lighting and correct shading. HUD elements (health bar, weapon icon, ammo) visible.

**What was wrong**: World geometry (walls, floors, ceiling) appeared as dark surfaces. Texture detail faintly visible but lighting was near-zero on environment.

**Proxy settings at this point**:
- SetTransform (slot 44): intercepted, blocked during FFP
- SetRenderState (slot 57): intercepted, blocks LIGHTING/CULLMODE/COLORVERTEX/SPECULARENABLE/AMBIENT/FOGENABLE during FFP
- SetMaterial (slot 49): intercepted, blocked during FFP
- SetTextureStageState (slot 67): intercepted, blocked during FFP
- FFP_Engage: forces wvpDirty=1, runs FFP_SetupLighting every draw
- FFP_ApplyTransforms: reads gameView/gameProj from memory, derives World = WVP * VP^-1

**rtx.conf at this point**:
- `rtx.fusedWorldViewMode = 0`
- `rtx.fallbackLightMode = 1` (distant light)
- `rtx.fallbackLightRadiance = 5.0 5.0 5.0`
- `rtx.zUp = False`

**Interpretation**: Transform decomposition is mathematically correct. The darkness on world geometry may be caused by: (a) ~1380 fullscreen quad draws creating camera-space geometry that blocks path-traced light, (b) insufficient fallback light intensity, or (c) texture/material classification issues.

### What Failed This Session

#### Failure 6: Screen-Space Draw Detection via c3 Heuristic

**Attempted**: Check `|c3.x| + |c3.y| + |c3.w| < 1.0` to detect projection-only WVP (screen-space draws). If detected, pass through with original shaders.

**Result**: Diagonal stripe artifacts. Remix misinterpreted shader-based draws, producing corrupted vertex data patterns across the screen.

**Lesson**: Cannot pass screen-space draws through with their original vertex shaders when Remix is active. Remix tries to capture vertex data from these draws and misinterprets it.

#### Failure 7: Skipping Screen-Space Draws Entirely

**Attempted**: Same c3 heuristic, but skip the draw entirely (`hr = 0; goto diag;`) instead of passing through.

**Result**: Rendering got worse — most visible geometry disappeared. The c3 heuristic was too aggressive and incorrectly classified some real 3D geometry draws as screen-space.

**Lesson**: The c3 column of WVP is NOT a reliable screen-space detector for TRL. Some 3D geometry has small c3.w values.

#### Failure 8: fallbackLightMode=2 (Point Light) with Radiance=10.0

**Attempted**: Changed from distant light (mode 1) to point light (mode 2) with higher radiance.

**Result**: No visible improvement. World geometry still dark.

### Current State of the Proxy (2026-03-17)

The deployed proxy is the "Lara working" build (reverted screen-space detection). Key interceptors are active (SetTransform, SetRenderState, SetMaterial, SetTextureStageState blocked during FFP). Per-draw transform and lighting reapplication.

### Config Conflict Alert

The handoff document previously recorded `rtx.fusedWorldViewMode = 1` in user.conf. The current rtx.conf has `rtx.fusedWorldViewMode = 0`. These have different meanings for Remix:
- **0**: None — Remix treats World/View/Projection independently
- **1**: View fused into World — Remix assumes World contains World*View combined
- **2**: World fused into View — Remix assumes View contains World*View combined

With the game-memory decomposition approach (separate W/V/P), mode 0 is correct. With the backup's WVP-only fallback (View=id, Proj=WVP, World=id), mode 0 is also what was being used. This setting has NOT been systematically tested across transform models.

#### Working Result 5: Full Scene Geometry (World + Lara) — WVP-as-World + fusedWorldViewMode=1

**Build**: WVP placed into D3DTS_WORLD, identity View/Projection. All interceptors active. Per-draw reapplication.

**rtx.conf**: `rtx.fusedWorldViewMode = 1`, `rtx.fallbackLightMode = 2`, `rtx.fallbackLightRadiance = 10.0 10.0 10.0`

**What was visible**: FULL SCENE — Lara, rock walls, ground, vines, rope, wooden structures, cave geometry. HUD visible. All geometry correctly positioned. First time both character AND world render together.

**What was wrong**: Path tracing not engaging. Scene renders as flat rasterized FFP output (no shadows, no reflections, no GI). Remix hooks but ray tracing appears inactive despite `rtx.enableRaytracing = True`.

**THE WINNING TRANSFORM MODEL**:
- `D3DTS_WORLD = WVP` (transposed from c0-c3 VS constants)
- `D3DTS_VIEW = identity`
- `D3DTS_PROJECTION = identity`
- `rtx.fusedWorldViewMode = 1` (tells Remix World contains World*View)

This is the simplest possible approach and it WORKS for geometry placement.

### Open Questions for Next Session

1. **Why is world geometry dark when Lara is lit?** Both use the same FFP path. The difference: Lara's WVP ≈ Projection (vertices pre-transformed to camera space); world geometry WVP includes full World*View*Projection. Could the World derivation produce normals/positions that block light?

2. **Are fullscreen quads creating blocking geometry?** 43% of draws are fullscreen quads. When FFP-converted, their World = View^-1 (camera-space geometry placed in world). These could cast shadows or occlude world surfaces. Need a reliable way to detect and handle them without the c3 heuristic.

3. **Would the backup's simpler WVP-only fallback work better?** View=identity, Proj=WVP, World=identity — no decomposition. Combined with `rtx.fusedWorldViewMode = 1`, Remix might handle this better than manual decomposition.

4. **Is WD_SetRenderState blocking too aggressively?** Alpha test/blend state might be frozen at wrong values, making some surfaces invisible. The proxy blocks changes to 9 render states during FFP but doesn't explicitly set alpha test/blend state.

5. **Does `DisableNormalMaps=1` affect this?** Currently enabled in proxy.ini but the proxy's SetTextureStageState blocking might conflict.

## 5. Experimental Backup Catalog

All test artifacts live in the game directory under `A:\SteamLibrary\steamapps\common\Tomb Raider LegendFIRSTVIBECODE\Reverse\`. Organization rules are in `Reverse/RULES.md`.

| Subfolder | Contents |
|-----------|----------|
| `Reverse/tests/` | Each test: `YYYYMMDD-HHMMSS-<Yes|No>-<Description>/` with d3d9.dll + proxy.ini + logs |
| `Reverse/builds/` | Old proxy DLL versions with timestamps |
| `Reverse/configs/` | Old proxy.ini, user.conf, rtx.conf backups |
| `Reverse/logs/ffp-proxy/` | FFP proxy logs |
| `Reverse/logs/dx-trace/` | DX9 tracer JSONL captures |
| `Reverse/logs/remix-runtime/` | Remix runtime logs (metrics, NRC) |

**Workflow**: Before deploying a new build, move the current d3d9.dll + proxy.ini + ffp_proxy.log into `Reverse/tests/` with the test result (Yes/No) and a description. Always check existing tests here before trying an approach — it may already have been tested.

This section catalogs every experiment so future sessions don't repeat failed approaches.

### Source Code Branches

Three distinct proxy code branches were identified across all backups:

| Branch | Location | DLL Size | Transform Strategy |
|--------|----------|----------|-------------------|
| A: Passthrough/fused-WVP | `patches/trl_legend/` | ~14KB | c0=WVP as World, View=id, Proj=id |
| B: Advanced decomposition | `patches/trl_legend_ffp/` | ~16KB | WVP@c0, World@c4, View@c8, ViewProj@c12, derives P from inverses |
| C: Experimental fused-WVP | `patches/TombRaiderLegend/` | ~18KB | WVP@c0, Proj@c4, View@c8, WorldView@c12, derives World from inversion |

### Backup Experiment Results

| # | Backup | Date | Result | DLL | Branch | proxy.ini Features |
|---|--------|------|--------|-----|--------|-------------------|
| 1 | `LIGHTBLUE2` | Mar 14 22:19 | LIGHT BLUE | 14,848 | A (passthrough) | DisableNormalMaps=1 |
| 2 | `ffFLASHINGLIGHTS` | Mar 14 22:26 | FLASHING LIGHTS | 16,384 | B | DisableNormalMaps=1, ForceFfpSkinned=1, ForceFfpNoTexcoord=1, FrustumPatch=1 |
| 3 | `LIGHTBLUE` | Mar 15 02:01 | LIGHT BLUE | 16,384 | B | Same as #2 (identical) |
| 4 | **`FIXEDFUNCTION`** | **Mar 15 03:45** | **WORKING** | **15,872** | **B** | **ALL features OFF** (DisableNormalMaps=0, ForceFfpSkinned=0, ForceFfpNoTexcoord=0, FrustumPatch=0) |
| 5 | `FLASHINGLIGHTS2` | Mar 15 03:48 | FLASHING LIGHTS | 20,480 | B (modified) | Same minimal as #4 |
| 6 | `BROKEN` (1st) | Mar 15 03:52 | BROKEN | 16,896 | B (modified) | Same minimal as #4 |
| 7 | `BROKEN` (2nd) | Mar 15 04:02 | BROKEN | 17,920 | B (modified) | Same minimal as #4 |
| 8 | `agent-restoreTRIANGLESLICES` | Mar 15 15:17 | TRIANGLE SLICES | 20,992 | B (agent-restored) | DisableNormalMaps=0, others OFF |
| 9 | `agent-passthroughLIGHTBLUE3` | Mar 15 15:22 | LIGHT BLUE | 13,824 | Pure passthrough | DisableNormalMaps=1 |
| 10 | **`agent-worldviewFIXEDFUNCTION2`** | **Mar 15 15:29** | **WORKING** | **18,944** | **B variant** | **ALL features OFF** + `rtx.conf` with fusedWorldViewMode=1, zUp=True, texture hash lists |
| 11 | `TRIANGLESLICES` | Mar 15 15:40 | TRIANGLE SLICES | 19,968 | B variant | DisableNormalMaps=0, others OFF |

### Visual Outcome Root Causes

**LIGHT BLUE** (backups #1, #3, #9): Remix hooks but receives no usable FFP geometry. Causes:
- Pure passthrough DLL (no FFP conversion attempted) — Remix shows default blue void
- Or advanced INI features (FrustumPatch, ForceFfpSkinned, ForceFfpNoTexcoord) interfere with draw routing

**FLASHING LIGHTS** (backups #2, #5): FFP active (`routedToFfp=1`) but with degenerate View/ViewProj matrices. Log from #5 shows c8-c11 had repeating values like `0.01, 0.19, 0.00, 0.00` across all rows — NOT valid camera matrices. The rapid transform oscillation causes visual strobing.

**BROKEN** (backups #6, #7): FFP eligibility failing. #6 log shows `Diag passthrough: 1` with `routedToFfp=0` on all draws — proxy logged state but did NOT convert to FFP. #7 shows `routedToFfp=0` even with passthrough=0 — eligibility criteria had a bug.

**TRIANGLE SLICES** (backups #8, #11): Partial/corrupted FFP transform. Vertex declaration stride or stream handling wrong, or incomplete matrix decomposition produces sheared geometry fragments.

**FIXEDFUNCTION** (backups #4, #10): Correct FFP rendering. See next section.

### What Made FIXEDFUNCTION Work

Both working backups share these properties:

1. **All proxy.ini advanced features OFF** — no FrustumPatch, no ForceFfpSkinned, no ForceFfpNoTexcoord, no DisableNormalMaps
2. **Branch B code** (advanced decomposition from `trl_legend_ffp`) — NOT the simple passthrough or the later experimental branch
3. **Log captured deep into gameplay** (drawCall ~320K) — c0-c3 had real 3D WVP matrices with large translations, c8-c15 had small but non-zero values
4. **FIXEDFUNCTION2 additionally had** `rtx.conf` with `rtx.fusedWorldViewMode=1`, `rtx.zUp=True`, and curated texture hash classifications

### proxy.ini: Features ON = Broken

This is the single clearest signal from all 11 experiments:

| proxy.ini Setting | When ON | When OFF |
|-------------------|---------|----------|
| `FrustumPatch=1` | LIGHTBLUE, FLASHINGLIGHTS | **FIXEDFUNCTION** |
| `ForceFfpSkinned=1` | LIGHTBLUE, FLASHINGLIGHTS | **FIXEDFUNCTION** |
| `ForceFfpNoTexcoord=1` | LIGHTBLUE, FLASHINGLIGHTS | **FIXEDFUNCTION** |
| `DisableNormalMaps=1` | LIGHTBLUE (passthrough) | **FIXEDFUNCTION** |

**Rule: Start with ALL features OFF. Enable only after base FFP is proven working.**

### Timestamped Log Evolution

The base directory contains 10 timestamped FFP proxy logs showing how the eligibility logic was iterated:

| Log | Timestamp | Key Finding |
|-----|-----------|-------------|
| `pre-211138` | Mar 15 20:07 | Simple DIP logging, no FFP routing fields |
| `pre-212108` | Mar 15 21:14 | Added wvp/world/view/viewProj labels. c8-c11 had real values (0.01-0.11) |
| `pre-212611` | Mar 15 21:22 | c8-c11 = ALL ZEROS, c12-c15 = ALL ZEROS (captured during menus) |
| `pre-223646` | Mar 15 22:32 | Boot only, no DIP data |
| `pre-224045` | Mar 15 22:37 | Added `rigidDecl`, `canUseFfp`, `usedFfp`. canUseFfp=0 because viewProjValid required but c8-c15 zeros |
| `pre-224614` | Mar 15 22:41 | Code changed: canUseFfp=1, usedFfp=1 despite zero c8-c15 |
| `pre-230757` | Mar 15 22:53 | Added `viewBlockNonZero` tracking. viewProjValid=0 → canUseFfp=0 |
| `pre-231238` | Mar 15 23:08 | Loosened: canUseFfp=1 even with viewProjValid=0 |
| `pre-004753` | Mar 17 00:47 | Renamed to `start0Seen`/`projectionReady`/`rigidMode`. FFP triggers when c0 written once |
| current | Mar 17 18:19 | Boot only, no DIP data |

**Key insight**: Most failed logs captured during menus/boot (5s or 50s delay), when c0-c3 has projection-like UI matrices and c8-c15 are zeros. The FIXEDFUNCTION success captured deep into 3D gameplay. The 50-second diagnostic delay is critical — too short catches menus, too long misses early geometry.

### Capture Timing Problem

| Backup | Draw Count at Capture | c0-c3 Content | c8-c15 Content | Outcome |
|--------|-----------------------|---------------|----------------|---------|
| FIXEDFUNCTION | ~320,000 | Real 3D WVP (large translations) | Small non-zero (0.01-0.19) | WORKING |
| FLASHINGLIGHTS2 | ~13,000 | WVP present | Degenerate repeating values | FLASHING |
| BROKEN (both) | ~12,000-13,000 | Projection-like (UI) | All zeros | BROKEN |

**Rule: Logs captured during menus are misleading. Always capture after entering a 3D level with visible world geometry.**

### Lessons for Future Sessions

1. **proxy.ini features are stability hazards** — FrustumPatch, ForceFfpSkinned, ForceFfpNoTexcoord all correlated with failure. Use only after base FFP works.
2. **c8-c15 are NOT view/projection matrices** — confirmed by CTAB analysis (Session 2026-03-17). They are fog, bend, and lighting constants. Previous register mapping assumptions were wrong.
3. **DLL size correlates with code complexity** — 13-15KB = passthrough/clean, 17-21KB = experimental/modified. Larger DLLs didn't improve results.
4. **The winning formula from FIXEDFUNCTION2** was: Branch B code + all features OFF + `rtx.fusedWorldViewMode=1` + `rtx.zUp=True` + curated texture hashes.
5. **The later session (2026-03-17) found an even simpler winning model**: WVP→D3DTS_WORLD, identity View/Proj, fusedWorldViewMode=1. This supersedes the Branch B decomposition approach.

## 6. Session 2026-03-17 (Evening): Hash Stability + Culling Removal Build

### Goal

Stable Remix hashes so placed lights stay fixed in world space when camera moves, and full culling removal so geometry is visible from all angles (360 around a placed light).

### Problem with Previous WVP-as-World Approach

Working Result 5 put WVP into D3DTS_WORLD with identity View/Projection and fusedWorldViewMode=1. This got full scene geometry visible but:
- **No path tracing** — Remix couldn't determine camera info because Projection was baked into World
- **Unstable hashes** — WVP changes every frame with camera, so every object gets a new hash when camera moves

### Changes Made

#### 1. FFP_ApplyTransforms: Full W/V/P Decomposition

Reads View and Projection from game memory at fixed addresses, decomposes WVP into separate matrices:

```
View from 0x010FC780 (row-major, updated by game per frame)
Proj from 0x01002530 (row-major, standard D3D perspective)
VP = View * Proj
World = WVP * inverse(VP)
```

Sets:
- `D3DTS_WORLD = World` (per-object, stable across camera movement)
- `D3DTS_VIEW = gameView` (camera transform from game memory)
- `D3DTS_PROJECTION = gameProj` (perspective projection from game memory)

Falls back to WVP-as-World if VP is not invertible (e.g. during menus).

#### 2. Fullscreen Quad Skip

43% of draws were fullscreen quads (primCount<=2, numVerts<=6). When FFP-converted with decomposed transforms, these create world-space blocking planes that occlude path-traced light. Now passed through with original shaders instead of FFP-converting.

#### 3. Global D3DCULL_NONE

Previously D3DCULL_NONE was only forced during FFP mode. Now ALL SetRenderState(CULLMODE) calls are intercepted and forced to D3DCULL_NONE regardless of FFP state. Combined with the existing in-memory patches:
- Frustum threshold at 0x00EFDD64 set to 1e30
- Cull-mode conditional at 0x0040EEA7 patched to always render

#### 4. rtx.conf Changes

- `rtx.fusedWorldViewMode = 0` (was 1) — Remix treats W/V/P independently since we provide proper separation
- `rtx.fallbackLightMode = 1` (was 0) — distant fallback light so scene isn't pitch black
- `rtx.fallbackLightRadiance = 5.0 5.0 5.0` — reasonable brightness for initial testing

#### 5. proxy.ini (All Features OFF)

```ini
[Remix]
Enabled=1
DLLName=d3d9.dll.bak

[FFP]
AlbedoStage=0
DisableNormalMaps=0
ForceFfpSkinned=0
ForceFfpNoTexcoord=0
FrustumPatch=0
```

### Deployment State

- `d3d9.dll` = 19,456 bytes (FFP proxy with decomposition)
- `d3d9.dll.bak` = d3d9_remix.dll (Remix bridge client, 2MB)
- `proxy.ini` = all features OFF
- `rtx.conf` = fusedWorldViewMode=0, fallbackLightMode=1
- Source: `patches/trl_legend_ffp/proxy/d3d9_device.c`

### Expected Behavior

- Remix receives proper World/View/Projection per draw
- Per-object World matrices are camera-independent → stable hashes
- Placed lights should stay fixed when camera moves
- All backface culling disabled → geometry visible from all angles
- Fullscreen quads pass through with shaders → no blocking geometry

### Test Instructions

1. Launch TRL, load into Bolivia (or any 3D level with visible geometry)
2. Wait 20+ seconds for proxy to initialize
3. Check if Remix renders path-traced geometry
4. In Remix developer menu: place a stage light on a piece of world geometry
5. Move Lara 360 degrees around the light — it should stay fixed in world space
6. If light moves with camera → decomposition or fusedWorldViewMode wrong
7. If scene is dark → check fallbackLightMode/radiance values
8. If geometry missing → check ffp_proxy.log after game exit

### Result: PATH TRACING WORKING (Working Result 6)

The shader-passthrough + transform override approach succeeded. Both screenshots confirm:
- Hash visualization shows distinct per-object hashes (colorful view)
- Path-traced rendering with real stone textures, shadows, ambient occlusion on world geometry
- Lara properly lit with path tracing
- Vegetation (ferns, plants) visible and textured
- Rock walls, ground, all world geometry present

**THE WINNING APPROACH — Shader Passthrough + Transform Override:**

The key insight: **don't convert to FFP at all**. The game's vertex shaders handle SHORT4 position transforms correctly. Remix's vertex capture (`rtx.useVertexCapture=True`) intercepts the post-VS output and uses the SetTransform W/V/P for world-space placement + camera setup.

What the proxy does:
1. **Keeps all shaders active** — does NOT null vertex or pixel shaders
2. **Overrides SetTransform** — sets decomposed World, gameView, gameProj before each draw
3. **Blocks dxwrapper's SetTransform** during active draws (dxwrapper sets identity V/P which would confuse Remix)
4. **Skips fullscreen quads** — `pc <= 2 && nv <= 6` draws are dropped entirely
5. **Forces D3DCULL_NONE** globally on all SetRenderState(CULLMODE) calls
6. **Applies in-memory patches** — frustum threshold at 0x00EFDD64 = 1e30, cull mode patch at 0x0040EEA7

Transform math:
```
gameView from 0x010FC780 (row-major View matrix from game memory)
gameProj from 0x01002530 (row-major Projection matrix from game memory)
VP = gameView * gameProj (cached once per BeginScene)
VP_inv = inverse(VP) (cached once per BeginScene)
World = transpose(vsConst[c0-c3]) * VP_inv (per draw)

D3DTS_WORLD = World (per-object, camera-independent)
D3DTS_VIEW = gameView (camera transform)
D3DTS_PROJECTION = gameProj (perspective projection)
```

Critical rtx.conf settings:
- `rtx.fusedWorldViewMode = 0` (W/V/P treated independently — REQUIRED for path tracing)
- `rtx.useVertexCapture = True` (captures post-VS positions)
- `rtx.zUp = True`
- `rtx.enableRaytracing = True`

proxy.ini settings (ALL features OFF):
```
AlbedoStage=0, DisableNormalMaps=0, ForceFfpSkinned=0, ForceFfpNoTexcoord=0, FrustumPatch=0
```

**Why previous FFP approaches failed:**
- SHORT4 vertex positions cannot be interpreted by Remix's FFP vertex capture
- Nulling shaders removed the only code that correctly transforms SHORT4 → clip space
- fusedWorldViewMode=1 prevented path tracing from engaging entirely
- Full decomposition with fusedWorldViewMode=0 produced noisy/dark world because FFP couldn't handle SHORT4

**Why this approach works:**
- Game's vertex shader handles SHORT4 → clip space natively
- Remix vertex capture intercepts post-VS output (already in clip space)
- Remix uses SetTransform W/V/P to reverse-map clip → world space for ray tracing
- Per-object World is camera-independent → stable hashes
- Proper View/Proj give Remix correct camera for ray casting

### Fix: Floating Polygon Artifacts (2026-03-19)

**Symptom**: Large shattered reflective polygon fragments floating throughout the 3D scene. White/mirror-like geometric shards at random angles, overlapping correct world geometry.

**Root Cause**: ~666 screen-space draws per frame (42% of all draws) were leaking through the draw routing filter into `FFP_Engage()`. These draws use **FLOAT3 POSITION** vertex declarations (post-processing, bloom, tonemapping, UI overlays). World geometry exclusively uses **SHORT4 POSITION**.

The previous fullscreen quad filter (`pc <= 2 && nv <= 6 && !curDeclHasNormal`) caught **zero** of these because TRL passes the entire shared vertex buffer capacity (~21845) as `NumVertices`, not the actual vertex count. The pre-transformed check (WVP ≈ Proj) also failed for some draws due to floating-point precision mismatch.

When these FLOAT3 draws entered `FFP_ApplyTransforms()`, the decomposition `World = WVP × inv(VP)` produced nonsensical transforms (their WVP is just a projection matrix, so `World = Proj × inv(VP)` = garbage), placing flat screen-aligned quads as random floating polygons in 3D space.

**Fix**: Replaced both the dead fullscreen quad check and the fragile pre-transformed heuristic with a single structural check: `curDeclPosIsFloat3`. This field was already parsed in `SetVertexDeclaration` and cleanly separates the two vertex format families. All FLOAT3-position draws are now skipped (`return 0`) before reaching `FFP_Engage()`.

**What was removed**:
- `pc <= 2 && nv <= 6 && !curDeclHasNormal` — caught zero draws in TRL
- WVP ≈ Proj matrix comparison (16-float per-draw comparison) — fragile, precision-dependent
- Orphaned `dip_done` label from the removed `goto`

### Remaining Issues

- **Wireframe grid lines on ground** — white lines at terrain patch/polygon boundaries. Attempted fixes: (1) `mat4_quantize` 1/256 grid on World matrix — caused visible grid artifacts, removed. (2) Suppressing line primitive draws (pt==2, pt==3) — had no effect, lines are NOT from line primitives. (3) Forcing D3DRS_FILLMODE=D3DFILL_SOLID before every draw call on the real device — still persists. The wireframe appears to be a Remix-side rendering artifact at mesh seams/boundaries under path tracing. May need `rtx.smoothNormalsTextures` with ground texture hashes, adjusting Remix denoiser settings, or using Remix geometry merging.
- **Hash instability** — `mat4_quantize` was removed. Replaced with cross-frame VP inverse caching (epsilon 1e-4 comparison: reuse previous frame's VP inverse when camera hasn't moved). This should stabilize hashes for static camera + static objects. If instability persists, the source may be vertex buffer contents or Remix hashing on additional state.
- **Lara outline in freecam** — when using Remix freecam, a ghost outline of Lara follows the camera center. Likely a game-side silhouette/outline shader effect. The game renders Lara relative to its internal camera, but Remix re-projects from the freecam position. Need to identify the outline draw's texture hash and add to `rtx.ignoreTextures`.
- **Stage light USD mod** — placed at `rtx-remix/mods/stagelight/mod.usda`. Red SphereLight (intensity=400) attached to mesh `46C470FAE2CCDB3E`. If the light doesn't appear, the mesh prim path may need adjustment via Remix DevTools.
- Some dark areas may need additional light sources via Remix toolkit

### If This Fails

- **Black/dark scene**: Increase `rtx.fallbackLightRadiance` or try `fallbackLightMode = 2` (point light)
- **Light moves with camera**: Decomposition math wrong or game-memory addresses shifted. Re-verify View/Proj addresses with livetools
- **No geometry at all**: VP inverse failing during gameplay. Check ffp_proxy.log for "fallback" messages
- **Diagonal stripes/corruption**: Fullscreen quad threshold too aggressive. Increase from `pc<=2 && nv<=6` to `pc<=4 && nv<=8`
- **Partial geometry**: Some draws not meeting FFP eligibility. Check if hasTexcoord or stride check is filtering real geometry
