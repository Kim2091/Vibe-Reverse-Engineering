# remix-comp

A DX9 proxy framework for RTX Remix compatibility mods, with built-in fixed-function pipeline (FFP) conversion. Part of the [Vibe Reverse Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) toolkit.

## What It Does

Legacy DX9 games use vertex/pixel shaders that RTX Remix can't inject ray-traced lighting into. This proxy sits between the game and Remix, intercepting D3D9 calls and converting shader-based rendering to fixed-function pipeline calls that Remix understands.

### Core Features

- **Full D3D9 proxy** — hooked IDirect3DDevice9 with every method detoured for interception
- **FFP conversion** — captures VS constants, parses vertex declarations, transposes matrices, and routes draw calls through the D3D9 fixed-function pipeline
- **Draw routing** — configurable decision trees that classify each draw call (3D geometry, HUD, skinned mesh) and decide whether to convert or pass through
- **INI configuration** — game-specific register layouts, albedo stage, skinning toggle, and diagnostics settings in `remix-comp.ini` (no recompile needed)
- **ImGui debug overlay** (F4) — live VS constant heatmap, matrix viewer, texture stage bindings, draw stats, FFP enable/disable toggle
- **Diagnostic logging** — timed frame dump to `rtx_comp\diagnostics.log` for debugging VS register layouts, vertex declarations, and draw call routing
- **Optional skinning module** — runtime-toggled vertex skinning with bone matrix upload, vertex buffer expansion, and compressed format decoding
- **Component module system** — `shared/` (game-agnostic static lib) + `comp/` (game-specific DLL) with clean separation
- **Per-game build split** — shared library stays in the base, only `comp/` is copied per game project

### Architecture

```
src/
  shared/              Game-agnostic static library
    common/
      config.hpp/cpp     INI config reader
      ffp_state.hpp/cpp  FFP state tracking, transforms, lighting, texture stages
      ...
    utils/               Hooking, memory, general utilities
  comp/                Game-specific DLL (copy this per game)
    main.cpp             DLL entry, window finding, config loading
    comp.cpp             Module registration
    game/                Game-specific patterns and structs
    modules/
      d3d9ex.cpp         D3D9 proxy with FFP + tracer interceptions
      renderer.cpp       Draw routing decision trees
      imgui.cpp          Debug overlay with FFP tab
      diagnostics.cpp    Frame logging
      skinning.cpp       Optional skinning
```

## Building

1. Run `generate-buildfiles_vs22.bat` to generate VS2022 project files
2. Open `build/remix-comp.sln` and build Release (x86)
3. Output: `build/bin/Release/remix-comp.asi`

For per-game projects, copy `src/comp/` to `patches/<GameName>/proxy/comp/` and use `premake5_game.lua.template`.

## Deploying

1. Copy `assets/dinput8.dll` (ASI Loader) to the game directory
2. Copy `remix-comp.asi` to `<game_dir>/plugins/`
3. Copy `assets/remix-comp.ini` to the game directory
4. Edit `remix-comp.ini` with game-specific register layout
5. Place `d3d9_remix.dll` (RTX Remix) in the game directory

## Contributors

| Who | What | Support |
|-----|------|---------|
| [xoxor4d](https://github.com/xoxor4d) | Original [remix-comp-base](https://github.com/xoxor4d/remix-comp-base) framework, D3D9 proxy architecture, ImGui integration, module system | [Ko-Fi](https://ko-fi.com/xoxor4d) / [Patreon](https://patreon.com/xoxor4d) |
| [kim2091](https://github.com/kim2091) | FFP conversion system, skinning module, diagnostic logging, INI config, toolkit integration | [Ko-Fi](https://ko-fi.com/kim20913944) |
| [momo5502](https://github.com/momo5502) | Initial codebase that remix-comp-base was built on | |

## Dependencies

- [Dear ImGui](https://github.com/ocornut/imgui) — debug overlay
- [MinHook](https://github.com/TsudaKageyu/minhook) — function hooking
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) — DLL injection
- [RTX Remix Bridge API](https://github.com/NVIDIAGameWorks/rtx-remix) — Remix integration

## License

MIT. See [LICENSE](LICENSE).
