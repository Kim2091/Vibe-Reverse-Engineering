# Tomb Raider Legend RTX Remix Definition Book

## Purpose
This file is the dictionary for the Tomb Raider Legend RTX Remix project. It defines the most important terms, files, addresses, config settings, register blocks, and log fields so another person can pick up the project without having to decode every hex value from scratch.

Where the meaning of a symbol is still incomplete, that is called out explicitly. Unknowns are part of the record and should not be hidden.

## Core Project Terms

| Term | Definition |
| --- | --- |
| TRL | Tomb Raider Legend |
| FFP | Fixed Function Pipeline |
| DXW | `dxwrapper.dll`, the D3D8-to-D3D9 translation layer used by TRL |
| Proxy | The custom `d3d9.dll` in `patches/trl_legend_ffp/proxy/` |
| Remix bridge | The DLL chain-loaded by the proxy via `DLLName=d3d9.dll.bak` |
| DXVK-Remix runtime | The renderer behind the bridge that performs RTX Remix rendering |
| Rigid path | The non-skinned rigid draw family currently targeted for FFP conversion |
| Skinned path | Character or bone-driven geometry paths that currently default to shader pass-through |
| Upstream | Game-side logic before proxy-side interpretation, especially before D3D8-to-D3D9 translation flattens intent |
| Helper family | The small set of upload wrappers and owner functions that repeatedly feed the active rigid path |
| Working state | The current repository state in which rigid `stride0=24` draws enter FFP and path tracing renders |

## Stack Terms

| Layer | Meaning |
| --- | --- |
| `trl.exe` | The original game executable |
| `dxwrapper.dll` | Makes the game appear D3D9-like by translating D3D8 calls and state |
| `d3d9.dll` proxy | Intercepts D3D9 device methods and performs the fixed-function conversion logic |
| `d3d9.dll.bak` | Current chain-load target used by the working proxy config |
| `user.conf` | RTX Remix runtime/user config with texture classifications and feature flags |
| `rtx.conf` | RTX Remix rendering config snapshot used with the current working setup |
| `ffp_proxy.log` | Runtime log written by the proxy for diagnostics and evidence |

## Important File Definitions

| Path | Definition |
| --- | --- |
| `patches/trl_legend_ffp/proxy/d3d9_device.c` | Main fixed-function conversion logic |
| `patches/trl_legend_ffp/proxy/d3d9_main.c` | Proxy entrypoint, logging, and chain loading |
| `patches/trl_legend_ffp/proxy/proxy.ini` | Proxy runtime config |
| `patches/trl_legend_ffp/proxy/build.bat` | Automated x86 build script for the proxy |
| `patches/trl_legend_ffp/sync_runtime_to_game.ps1` | Automated runtime deployment script |
| `patches/trl_legend_ffp/kb.h` | Active project knowledge base |
| `TOMB_RAIDER_LEGEND_RTX_REMIX_HANDOFF.md` | Historical handoff summary before the latest success |
| `patches/trl_legend_ffp/upstream_camera_capture.jsonl` | Wide trace of candidate camera/projection upload owners |
| `patches/trl_legend_ffp/wrapper_callers_capture.jsonl` | Trace proving which upload wrappers dominate the active rigid path |
| `patches/trl_legend_ffp/matrix_owner_capture.jsonl` | Trace proving who owns the active `start=0` projection-like path |

## Graphics API Terms

| Term | Definition |
| --- | --- |
| `D3DTS_WORLD` | The fixed-function world transform slot |
| `D3DTS_VIEW` | The fixed-function view/camera transform slot |
| `D3DTS_PROJECTION` | The fixed-function projection transform slot |
| `SetVertexShaderConstantF` | The D3D9 call used to upload float shader constants |
| `DrawIndexedPrimitive` | The draw call the proxy mainly intercepts for rigid world rendering |
| Vertex declaration | The layout description for vertex data; used by the proxy to recognize rigid vs skinned paths |
| `stride0=24` | The currently targeted rigid declaration path; stream 0 stride is 24 bytes |

## Working Config Definitions

### `proxy.ini`

| Key | Working value | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Enables chain loading into RTX Remix |
| `DLLName` | `d3d9.dll.bak` | Current working chain-load target |
| `AlbedoStage` | `0` | Stage 0 is treated as the main diffuse/albedo texture |
| `DisableNormalMaps` | `1` | Disables non-albedo stages during FFP draws |
| `ForceFfpSkinned` | `0` | Skinned meshes still default to shader pass-through |
| `ForceFfpNoTexcoord` | `0` | Rigid FFP path still expects texcoords on the targeted declaration |

### `dxwrapper.ini`

| Key | Working value | Meaning |
| --- | --- | --- |
| `D3d8to9` | `1` | Tomb Raider Legend is translated from D3D8 to D3D9 before the proxy sees it |

### `user.conf` and `rtx.conf`

| Key | Working value | Meaning |
| --- | --- | --- |
| `rtx.useVertexCapture` | `True` | Enables vertex capture in Remix |
| `rtx.enableRaytracing` | `True` | Enables path tracing |
| `rtx.zUp` | `True` | Working scene orientation assumption |
| `rtx.orthographicIsUI` | `True` | Helps UI classification |
| `rtx.fusedWorldViewMode` | `2` | Present in the known-good runtime state; keep until disproven |
| `rtx.worldSpaceUiTextures` and related lists | many values | Curated hash classification used to stabilize Remix interpretation |

## Log Field Definitions

| Field | Definition |
| --- | --- |
| `start0Seen` | The proxy has observed the active `start=0` upload family |
| `projectionReady` | The proxy validated that the upstream projection matrix at `0x01002530` looks usable |
| `rigidDecl` | The current draw matches the targeted rigid `stride0=24` declaration layout |
| `canUseFfp` | The proxy's current routing logic says the draw is eligible for FFP conversion |
| `usedFfp` | The proxy actually entered FFP mode for the draw |
| `c0(start0)` | The captured `start=0` 4x4 block |
| `c4(c4-7)` | The captured companion block at `c4-c7` |
| `c8(c8-11)` | The captured first half of the rare auxiliary block |
| `c12(c12-15)` | The captured second half of the rare auxiliary block |

## Register Block Definitions

These definitions are about the active rigid path, not necessarily every draw family in the game.

| Register range | Current meaning | Confidence |
| --- | --- | --- |
| `c0-c3` | Projection-like 4x4 written by `BuildAndUploadStart0Matrix()` | High |
| `c4-c7` | Companion data block; not currently used as authoritative world transform in the working path | Medium |
| `c6` | Scalar companion written by `UploadScalarRegisterC6IfChanged()` | High for existence, incomplete for semantic meaning |
| `c8-c15` | Rare auxiliary/frustum-style block written by `UploadAuxBlockC8FromGlobals()` | High that it is not the active rigid-path camera block |
| `c28` | Companion vec3 written by `UploadScalarRegisterC28()` | High for existence, incomplete for full semantic meaning |

## Function And Address Glossary

### High-Level Legacy Candidates

| Address | Current name | Definition |
| --- | --- | --- |
| `0x00ECBA40` | `SetVertexShaderConstantF` helper | Thin wrapper around the actual VS constant upload call |
| `0x00ECBB00` | `UploadMatrixBlocks` | Older matrix upload path that writes two 8-register blocks |
| `0x0060C7D0` | unresolved gameplay pass | Originally suspected to own the camera path; not dominant on the active rigid trace |
| `0x0060EBF0` | unresolved gameplay pass | Originally suspected to own active uploads; still worth future study |
| `0x00610850` | unresolved gameplay pass | Originally suspected to own active uploads; still worth future study |

### Active Helper Family

| Address | Current name | Definition |
| --- | --- | --- |
| `0x00413950` | `Upload4x4AtRegister` | Transposes a 4x4 matrix and uploads it to an arbitrary register start |
| `0x00413BF0` | `UploadScalarRegisterC6IfChanged` | Uploads a scalar companion value through `start=6,count=1` if it changed |
| `0x00413F40` | `UploadScalarRegisterC28` | Uploads a vec3/companion value through `start=28,count=1` |
| `0x00413F80` | `UploadAuxBlockC8FromGlobals` | Uploads the rare `start=8,count=8` auxiliary block from globals |
| `0x00415040` | `BuildAndUploadStart0Matrix` | Builds the projection-like `start=0` matrix and pushes it through `Upload4x4AtRegister(0, ...)` |
| `0x00415260` | `UploadFrameProjectionAndAuxBlock` | Frame-level path that refreshes the `start=8` auxiliary block and one `start=0` matrix |
| `0x00415AB0` | `SubmitProjectionDrivenRigidDraw` | Dominant rigid draw owner that refreshes `start=0` and `c6` when cached source fields change |

### Important Globals

| Address | Current name | Definition |
| --- | --- | --- |
| `0x01002530` | `g_upstreamProjectionMatrix` | Authoritative row-major projection matrix used by the active rigid path before helper transposition |
| `0x010FA280` | `g_c8AuxVectorsXYZ` | Start of the auxiliary vector block used by `UploadAuxBlockC8FromGlobals()` |
| `0x010024BC` | opaque proxy-side state object | Passed into `0x00ECBA60`; exact meaning still incomplete |
| `0x010024C0` | opaque proxy-side state object | Alternate object passed into `0x00ECBA60`; exact meaning still incomplete |

## Structure Glossary

### `TrlRenderContext`
This structure is relevant to the older paired-block upload path:

| Offset | Field | Meaning |
| --- | --- | --- |
| `+0x0C` | `device` | D3D device pointer |
| `+0x480` | `projection` | Projection matrix used by the older block upload path |
| `+0x4C0` | `view` | View matrix used by the older block upload path |
| `+0x500` | `viewProjection` | View-projection matrix used by the older block upload path |
| `+0x540` | `world` | World matrix used by the older block upload path |
| `+0x580` | `dirtyViewProj` | Dirty flag |
| `+0x581` | `dirtyWorldWvp` | Dirty flag |

### `TrlProjectionDrawItem`
This structure describes the active rigid per-draw owner fields used by `0x00415AB0`.

| Offset | Field | Meaning |
| --- | --- | --- |
| `+0x00` | `cacheKey` | Compared before refreshing companion values |
| `+0x18` | `companionVec3` | Source for `UploadScalarRegisterC28()` |
| `+0x1C` | `primitiveCount` | Tested before issuing the draw |
| `+0x24` | `c6Value` | Source for the `start=6,count=1` upload |
| `+0x28` | `start0ZOffset` | Source for `BuildAndUploadStart0Matrix()` |

## Artifact Glossary

| Artifact | Definition |
| --- | --- |
| `trace_vsconst_hist.jsonl` | Early histogram proving which upload starts dominate |
| `trace_reg0.jsonl` | Early proof that `start=0` is overloaded and not always one transform kind |
| `upstream_camera_capture.jsonl` | Broad capture used to classify the upstream helper family |
| `wrapper_callers_capture.jsonl` | Focused capture used to prove the dominant helper wrappers |
| `matrix_owner_capture.jsonl` | Focused capture used to prove the dominant owner of the active `start=0` path |
| `ffp_proxy.log` | Proxy-written runtime proof of whether rigid draws really entered FFP |

## Reverse Engineering Terms Used In This Project

| Term | Project-specific meaning |
| --- | --- |
| upstream | Earlier in the game's ownership chain, before the proxy's interpretation |
| active rigid path | The draw family currently known to enter FFP successfully |
| auxiliary block | Data that is real and meaningful, but not the camera source the proxy should trust |
| projection-driven path | A path where a valid projection source is known, but full view/world ownership is not yet cleanly mapped |
| handoff | A deliberate transfer of validated upstream data into proxy-side logic |
| cache key | A field used by the owner function to avoid redundant uploads |

## Known Unknowns
These items are intentionally left unresolved and should be documented as such:

1. The exact semantic meaning of the `c6` companion scalar beyond "active and important."
2. The exact semantic meaning of the `c28` companion vec3 beyond "active and important."
3. Whether `0x010024BC` and `0x010024C0` are best described as state-object factories, transform selectors, or something else.
4. The cleanest authoritative gameplay `VIEW` source for the active camera.
5. The cleanest authoritative gameplay `WORLD` source for the rigid path.

## Reading Advice
Use this file as a decoder ring:

- If you see an address in the proxy, look it up here.
- If you see a config key in `user.conf` or `proxy.ini`, look it up here.
- If you see a log field in `ffp_proxy.log`, look it up here.
- If you are continuing the reverse engineering work, open the roadmap next.
