---
name: 'renderdoc-analysis'
description: 'RenderDoc GPU capture analysis. Use for .rdc files, draw call inspection, pipeline state, textures/shaders, mesh data, GPU counters.'
---

# RenderDoc Analysis

All commands: `python -m renderdoctools <command> [args]`. All support `--json` and `--output FILE`. **DX9 is NOT supported** — use dx9tracer instead.

## Commands

| Command | Purpose |
|---------|---------|
| `events <rdc> [--draws-only]` | List events/draw calls |
| `analyze <rdc> --summary` | Capture overview stats |
| `analyze <rdc> --biggest-draws N` | Top N draws by vertex count |
| `analyze <rdc> --render-targets` | Unique render targets |
| `pipeline <rdc> --event EID` | Pipeline state (optional `--stage pixel`) |
| `textures <rdc> --event EID [--save-all DIR]` | Bound textures, export |
| `shaders <rdc> --event EID [--cbuffers]` | Shader disassembly + constants |
| `mesh <rdc> --event EID [--post-vs]` | Vertex data |
| `descriptors <rdc> --event EID [--type srv]` | Descriptor bindings |
| `api-calls <rdc> [--event EID] [--filter NAME]` | API call params |
| `counters <rdc> [--zero-samples]` | GPU counters, find wasted draws |
| `pixel-history <rdc> --event EID --resource RID --x X --y Y` | What drew to pixel? |
| `pick-pixel <rdc> --resource RID --x X --y Y` | Read pixel value |
| `usage <rdc> --resource RID [--filter read\|write]` | Resource usage across events |
| `debug-shader <rdc> --event EID --mode vertex\|pixel --vertex-index N` | Debug shader execution |
| `open <rdc>` | Launch RenderDoc GUI |

## Finding the Right Draw Call

1. **Render targets**: `analyze --render-targets` → dump RTs → `usage --resource <RID> --filter write` → narrow by EID
2. **Binary search**: `events --draws-only`, pick midpoint, dump RT, converge
3. **By name**: `events --filter "shadow"` (if engine uses debug markers)
4. **By size**: `analyze --biggest-draws 20`

## Verification

Always confirm you're looking at the right thing:
- Dump and visually check RTs: `textures --event <EID> --save-all ./verify`
- Spot-check pixels: `pick-pixel --resource <RID> --x 100 --y 100`
- Compare before/after EIDs

## Multi-Pass Reconstruction

1. `analyze --render-targets` — list all RTs
2. Per RT: `usage --resource <RID>` — writes = pass boundaries, reads = consumers
3. Dump textures at key EIDs to label passes (shadow, GBuffer, lighting, post)

## When to Use GUI

`python -m renderdoctools open <rdc>` — better for texture scrubbing, 3D mesh viewer, shader debugger with source, overlay modes (wireframe, depth, overdraw).
