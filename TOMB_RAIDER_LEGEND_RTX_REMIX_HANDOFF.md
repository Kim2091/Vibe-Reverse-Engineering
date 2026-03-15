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
