# Tomb Raider Legend RTX Remix Documentation Index

## Purpose
This document is the entry point for the current Tomb Raider Legend RTX Remix effort in this repository. It summarizes what is working, what changed to make it work, where the hard evidence lives, and which companion documents to read next.

The immediate milestone reached by this repository is:

1. `trl.exe` launches through `dxwrapper.dll`, then through the custom `d3d9.dll` proxy, then into RTX Remix.
2. The proxy now enters fixed-function mode on the traced rigid `stride0=24` draw family.
3. The proxy uses the upstream projection matrix at `0x01002530` instead of waiting for the old zeroed `c8-c15` camera block.
4. The runtime log from the successful run shows `projectionReady=1`, `canUseFfp=1`, and `usedFfp=1` on rigid draws.

This is a working path-tracing-capable state for the rigid draw family. It is not the end of the reverse engineering project. The true gameplay `VIEW` and `WORLD` ownership still need to be fully mapped if the goal is a cleaner and more complete fixed-function port.

## Documentation Map
Read these in order if you want the shortest path from zero context to productive work:

1. `docs/TRL-RTX-Remix-Paper.md`
   This is the detailed technical paper. It explains the investigation history, the failed branches, the successful pivot, the evidence, and why the current solution works.

2. `docs/TRL-RTX-Remix-Project-Setup.md`
   This is the reproducible setup and operations guide. It explains the build, sync, launch, configuration, validation, and troubleshooting process.

3. `docs/TRL-RTX-Remix-Definition-Book.md`
   This is the glossary and address book. It defines the important functions, globals, register blocks, config values, artifacts, and log fields used throughout the project.

4. `docs/TRL-RTX-Remix-RE-Roadmap.md`
   This is the forward plan for continuing the reverse engineering effort from the current working state to a more complete Tomb Raider Legend fixed-function conversion.

5. `docs/TRL-RTX-Remix-Experiment-Log.md`
   This is the chronological lab notebook. It records what was tried, why it was tried, what failed, and what finally worked.

## Working Architecture
```mermaid
flowchart LR
    TRL["trl.exe"] --> DXW["dxwrapper.dll"]
    DXW --> D8TO9["D3D8 -> D3D9 translation"]
    D8TO9 --> PROXY["custom d3d9.dll proxy"]
    PROXY --> BRIDGE["d3d9.dll.bak / RTX Remix bridge"]
    BRIDGE --> REMIX["DXVK-Remix runtime"]

    SUBGRAPH1["Traced active rigid path"]
    START0["0x01002530 upstream projection matrix"] --> BUILD0["0x00415040 BuildAndUploadStart0Matrix"]
    BUILD0 --> UP4X4["0x00413950 Upload4x4AtRegister(start=0)"]
    UP4X4 --> PROXY
    C6["0x00413BF0 UploadScalarRegisterC6IfChanged"] --> PROXY
    C28["0x00413F40 UploadScalarRegisterC28"] --> PROXY
    AUX["0x00413F80 UploadAuxBlockC8FromGlobals"] --> PROXY
    end
```

## What Finally Changed
The successful change was not "make Remix smarter." It was "stop assuming the wrong camera source."

The proxy previously treated the active `c0-c3` path as a fused world-view-projection candidate or waited for the `c8-c15` block to become a valid camera source. The traced rigid path proved that:

- `c8-c15` was often zero on the rigid draw family that we actually wanted.
- `start=0,count=4` was active and stable enough to act as the projection side of the transform problem.
- The authoritative upstream projection matrix existed in game memory at `0x01002530` before the upload helper transposed it into shader constants.

The fix was to:

1. Gate FFP entry on the active rigid declaration path plus the presence of a valid upstream projection matrix.
2. Read `0x01002530` directly and feed it into `D3DTS_PROJECTION`.
3. Stop depending on the old `c8-c15` camera block for the working rigid path.
4. Keep `WORLD` and `VIEW` as identity on that path until the remaining upstream ownership is mapped cleanly.

## Decisive Runtime Evidence
The working run produced the following proxy state in `A:\SteamLibrary\steamapps\common\Tomb Raider Legend\ffp_proxy.log`:

- `start0Seen=1`
- `projectionReady=1`
- `stride0=24`
- `rigidDecl=1`
- `canUseFfp=1`
- `usedFfp=1`

Those fields are the simplest proof that the proxy is no longer stuck behind the old camera-detection failure path.

## Artifact Inventory
These files are the most important evidence and implementation artifacts in the repo right now.

| Path | Role | Why it matters |
| --- | --- | --- |
| `patches/trl_legend_ffp/proxy/d3d9_device.c` | Active proxy logic | Contains the working upstream-projection feed and rigid-path FFP routing |
| `patches/trl_legend_ffp/kb.h` | Knowledge base | Records the recovered helper family, globals, and traced address meanings |
| `patches/trl_legend_ffp/proxy/build.bat` | Build automation | Rebuilds the proxy automatically with MSVC x86 |
| `patches/trl_legend_ffp/sync_runtime_to_game.ps1` | Deployment automation | Pushes the latest runtime files into the live game directory |
| `patches/trl_legend_ffp/upstream_camera_capture.jsonl` | Wide caller trace | Captures the upstream helper family around the camera/projection pivot |
| `patches/trl_legend_ffp/wrapper_callers_capture.jsonl` | Wrapper caller trace | Proves which tiny upload wrappers dominate the active rigid path |
| `patches/trl_legend_ffp/matrix_owner_capture.jsonl` | Matrix owner trace | Proves who is feeding the active `start=0` matrix path |
| `A:\SteamLibrary\steamapps\common\Tomb Raider Legend\ffp_proxy.log` | Working runtime log | Shows `projectionReady=1` and `usedFfp=1` on rigid draws |
| `A:\SteamLibrary\steamapps\common\Tomb Raider Legend\user.conf` | Working Remix config | Contains the known-good RTX Remix feature and classification settings |
| `A:\SteamLibrary\steamapps\common\Tomb Raider Legend\dxwrapper.ini` | Translation layer config | Confirms `D3d8to9 = 1`, which explains the entire project shape |
| `TOMB_RAIDER_LEGEND_RTX_REMIX_HANDOFF.md` | Historical summary | Captures earlier branches, failures, and context before the latest success |

## Key Working Assumptions
These are the assumptions the current successful build is making:

1. Tomb Raider Legend is effectively a D3D8 renderer translated into D3D9 state by `dxwrapper.dll`.
2. The rigid `stride0=24` declaration path is the first draw family worth converting.
3. The active rigid path exposes a stable upstream projection matrix earlier than it exposes a trustworthy upstream view matrix.
4. Using the upstream projection matrix directly is better than waiting for a camera block that never becomes nonzero on this draw family.
5. The current result is a valid working milestone even though the full view/world ownership story is not finished yet.

## Recommended Next Reading Paths
Choose one of these based on your goal:

- If you want the full story and the reasoning behind the success, read `docs/TRL-RTX-Remix-Paper.md`.
- If you want to rebuild, sync, launch, and validate immediately, read `docs/TRL-RTX-Remix-Project-Setup.md`.
- If you want a dictionary of addresses, configs, and function meanings, read `docs/TRL-RTX-Remix-Definition-Book.md`.
- If you want to continue reverse engineering toward a more complete fixed-function conversion, read `docs/TRL-RTX-Remix-RE-Roadmap.md`.

## Additional Documentation Worth Adding Later
These are not required for the current milestone, but they would make the project easier to hand off and scale:

- A level-by-level coverage matrix listing which draw families have been validated in which gameplay areas.
- A texture-classification workbook explaining every curated hash list in `user.conf`.
- A symbol pack that promotes the important recovered addresses into a more complete KB header or Ghidra script.
- A "known-good runtime snapshot" checklist containing hashes, timestamps, and copies of the exact deployed runtime files.
- A dedicated skinned-mesh investigation notebook that separates character work from rigid-world work.
