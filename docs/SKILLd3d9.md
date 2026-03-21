---
name: direct3d9-graphics
description: >
  Comprehensive Direct3D 9 graphics programming reference and guidance. Use this skill whenever the user asks about D3D9, DirectX 9, fixed-function pipeline, programmable shaders (vertex/pixel shaders in D3D9 context), IDirect3DDevice9, IDirect3D9, render states, texture stages, vertex/index buffers, D3DX utilities, HLSL effects in D3D9, or the D3D9 graphics pipeline. Also trigger for RTX Remix modding questions involving D3D9 interception, DLL interposition of d3d9.dll, geometry hashing of D3D9 draw calls, fixed-function vs shader pipeline compatibility, or game compatibility analysis. Trigger for any COM interface starting with IDirect3D, any D3D9 structure (D3DPRESENT_PARAMETERS, D3DVERTEXELEMENT9, D3DLIGHT9, D3DMATERIAL9, etc.), any D3D9 enumeration (D3DFORMAT, D3DPOOL, D3DUSAGE, D3DRENDERSTATETYPE, etc.), D3D9 device creation, swap chains, surfaces, or depth/stencil buffers. Use even if the user only says "DirectX" without specifying version when context suggests legacy/retro game development.
---

# Direct3D 9 Graphics — Claude Skill

This skill provides comprehensive knowledge of the Microsoft Direct3D 9 API, covering the full graphics pipeline from device creation through final pixel output. It integrates the official Microsoft documentation structure with practical RTX Remix modding context.

## When to read reference files

Before answering, check which domain the question falls into and read the appropriate reference:

| Question domain | Reference file to read |
|---|---|
| Pipeline architecture, HAL vs REF devices, system integration, device creation, swap chains, presentation | `references/pipeline-architecture.md` |
| COM interfaces (IDirect3D9, IDirect3DDevice9, etc.), methods, structures, enumerations, constants, D3DX | `references/interfaces-reference.md` |
| Fixed-function pipeline vs programmable shaders, when each applies, T&L, FFP render states, shader models | `references/ffp-vs-shaders.md` |
| Rendering, render states, texture stage states, alpha blending, fog, depth buffers, effects framework, HLSL | `references/rendering-states.md` |
| RTX Remix interception of D3D9, hashing, game compatibility, DLL interposition, scene reconstruction | `references/rtx-remix-integration.md` |

For complex questions, read multiple reference files. For questions spanning the whole API, start with `pipeline-architecture.md`.

## Quick orientation: What is Direct3D 9?

Direct3D 9 is Microsoft's COM-based 3D graphics API released with DirectX 9.0 (2002), with the 9.0c update (2004) adding Shader Model 3.0. It exposes the GPU through a Hardware Abstraction Layer (HAL) device and provides two rendering paradigms:

**Fixed-Function Pipeline (FFP):** The legacy path where the application configures rendering via render states, texture stage states, transformation matrices, lights, and materials — no shader code. The GPU's built-in T&L (Transform & Lighting) unit handles vertex processing. This is the pipeline that RTX Remix can intercept and replace with path tracing.

**Programmable Shader Pipeline:** Vertex shaders (VS) and pixel shaders (PS) replace parts of the fixed-function pipeline with custom GPU programs. Shader Model 1.x–3.0 are supported. Games using shaders extensively cannot be fully intercepted by RTX Remix because the rendering logic is opaque to external tools.

The API is accessed through COM interfaces rooted at `IDirect3D9` (enumeration and device creation) and `IDirect3DDevice9` (all rendering operations). Resources include vertex buffers, index buffers, textures (2D, cube, volume), surfaces, and state blocks.

## Core documentation URL structure

All official Microsoft D3D9 documentation lives under:
`https://learn.microsoft.com/en-us/windows/win32/direct3d9/`

Key entry points:
- Programming Guide: `.../dx9-graphics-programming-guide`
- Getting Started: `.../getting-started` (Architecture, Devices, Resources, Transforms, Lights, Rendering, Textures)
- Advanced Topics: `.../advanced-topics` (Vertex Pipeline, Pixel Pipeline, Frame Buffer, HDR, PRT, Queries)
- Effects: `.../effects` (Writing/Using effects, effect states)
- Programming Tips: `.../programming-tips` (Performance, Multithreading, Debug, Multihead)
- API Reference: `.../dx9-graphics-reference` → Interfaces, Functions, Structures, Enumerations, Constants
- D3DX Reference: `.../dx9-graphics-reference-d3dx` (Math, Mesh, Texture, Effect utilities)

## Response guidelines

1. **Always cite the specific Microsoft docs URL** when referencing API methods, structures, or behaviors. Construct URLs from the pattern above.
2. **Distinguish FFP from shader pipeline** — many D3D9 concepts only apply to one path. Always clarify which pipeline context applies.
3. **Include COM interface method signatures** when discussing specific API calls (interface name, method name, key parameters).
4. **For RTX Remix questions**, read `references/rtx-remix-integration.md` and cross-reference with the D3D9 pipeline knowledge to explain *why* certain games are compatible or incompatible.
5. **For code examples**, use C++ with the D3D9 API conventions (COM pointers, HRESULT checks, LPDIRECT3D typedefs).
6. When the user asks about a specific game's D3D9 usage, analyze whether it uses FFP or shaders and what that means for tools like RTX Remix, dgVoodoo2, DXVK, or ReShade.
