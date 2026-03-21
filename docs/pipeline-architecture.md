# Direct3D 9 Pipeline Architecture

## Table of Contents
1. [System Integration](#system-integration)
2. [Graphics Pipeline Stages](#graphics-pipeline-stages)
3. [Device Types and Creation](#device-types-and-creation)
4. [Swap Chains and Presentation](#swap-chains-and-presentation)
5. [Resource Management](#resource-management)
6. [Coordinate Systems and Transforms](#coordinate-systems-and-transforms)
7. [Viewports and Clipping](#viewports-and-clipping)

---

## System Integration

Direct3D 9 sits between the application and the graphics hardware via a device-independent COM interface. The key relationships:

- **Application** → calls D3D9 COM interfaces
- **Direct3D Runtime** (`d3d9.dll`) → mediates between app and driver
- **HAL Device** → maps D3D9 calls to actual GPU hardware via the device driver
- **REF Device** → software-only reference rasterizer for testing (not for shipping)
- **GDI** → coexists alongside D3D9; both access hardware through the driver

Direct3D does NOT use GDI for rendering — it has its own path to the hardware. However, GDI and D3D9 can share the same display. The runtime loads from `d3d9.dll` via `Direct3DCreate9()` or `Direct3DCreate9Ex()` (Vista+).

This DLL-based architecture is precisely what RTX Remix exploits: by placing a custom `d3d9.dll` next to the game executable, it intercepts all D3D9 calls via DLL search order (DLL interposition).

## Graphics Pipeline Stages

The D3D9 pipeline processes geometry through these ordered stages:

### 1. Input Assembly
- **Vertex Data**: Stored in vertex buffers (`IDirect3DVertexBuffer9`). Vertices contain position plus optional normals, colors, texture coordinates, blend weights, etc.
- **Index Data**: Stored in index buffers (`IDirect3DIndexBuffer9`). 16-bit or 32-bit indices reference vertices for efficient triangle sharing.
- **Vertex Declaration** (`IDirect3DVertexDeclaration9`): Describes the layout of vertex data — which elements exist, their types, offsets, and streams. Replaces legacy FVF (Flexible Vertex Format) codes but FVF is still supported for convenience.
- **Primitive Types**: `D3DPT_POINTLIST`, `D3DPT_LINELIST`, `D3DPT_LINESTRIP`, `D3DPT_TRIANGLELIST`, `D3DPT_TRIANGLESTRIP`, `D3DPT_TRIANGLEFAN`.

### 2. Tessellation (Optional)
Higher-order surfaces (N-patches, rect patches, tri patches) and displacement maps are tessellated into vertex locations before vertex processing.

### 3. Vertex Processing
Two mutually exclusive paths:
- **Fixed-Function T&L**: World/view/projection transforms via `SetTransform()`, lighting via `SetLight()`/`SetMaterial()`, fog computation. Controlled entirely by render states.
- **Vertex Shaders**: Custom vertex programs (VS 1.1 through VS 3.0) that replace the entire fixed-function vertex pipeline. Set via `SetVertexShader()`.

### 4. Geometry Processing (Rasterizer)
- Back-face culling (`D3DRS_CULLMODE`)
- Clipping to the view frustum
- Homogeneous divide and viewport mapping
- Triangle setup and rasterization
- Attribute interpolation across triangle faces

### 5. Pixel Processing
Two mutually exclusive paths:
- **Fixed-Function Texture Stages**: Up to 8 texture stages configured via `SetTextureStageState()` and `SetSamplerState()`. Each stage combines texture samples with previous results using configurable operations.
- **Pixel Shaders**: Custom pixel programs (PS 1.1 through PS 3.0) that replace texture stage processing. Set via `SetPixelShader()`.

### 6. Output Merger
- **Alpha Testing**: `D3DRS_ALPHATESTENABLE`, `D3DRS_ALPHAREF`, `D3DRS_ALPHAFUNC`
- **Depth Testing**: `D3DRS_ZENABLE`, `D3DRS_ZFUNC`, `D3DRS_ZWRITEENABLE`
- **Stencil Testing**: `D3DRS_STENCILENABLE` and related states
- **Alpha Blending**: `D3DRS_ALPHABLENDENABLE`, `D3DRS_SRCBLEND`, `D3DRS_DESTBLEND`
- **Fog Blending**: Vertex or pixel fog applied as final color modification
- **Dithering**: `D3DRS_DITHERENABLE`
- Write to the render target surface and depth/stencil surface

## Device Types and Creation

### Creation Flow
```
Direct3DCreate9(D3D_SDK_VERSION) → IDirect3D9*
  → GetAdapterCount(), GetAdapterIdentifier(), GetDeviceCaps()
  → CheckDeviceType(), CheckDeviceFormat(), CheckDeviceMultiSampleType()
  → CreateDevice() → IDirect3DDevice9*
```

### D3DPRESENT_PARAMETERS
Critical structure for device creation:
- `BackBufferWidth`, `BackBufferHeight` — render target dimensions
- `BackBufferFormat` — pixel format (D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, etc.)
- `BackBufferCount` — number of back buffers (1–3)
- `MultiSampleType` — MSAA level
- `SwapEffect` — D3DSWAPEFFECT_DISCARD (most common), _FLIP, _COPY
- `hDeviceWindow` — target HWND
- `Windowed` — TRUE for windowed, FALSE for fullscreen
- `EnableAutoDepthStencil` — auto-create depth buffer
- `AutoDepthStencilFormat` — D3DFMT_D24S8, D3DFMT_D16, etc.
- `PresentationInterval` — vsync control (D3DPRESENT_INTERVAL_ONE, _IMMEDIATE, etc.)

### Device Behavior Flags
- `D3DCREATE_HARDWARE_VERTEXPROCESSING` — GPU handles vertex processing
- `D3DCREATE_SOFTWARE_VERTEXPROCESSING` — CPU handles vertex processing
- `D3DCREATE_MIXED_VERTEXPROCESSING` — switch between HW and SW at runtime
- `D3DCREATE_MULTITHREADED` — thread-safe device (performance cost)
- `D3DCREATE_FPU_PRESERVE` — don't change FPU precision

### D3DCAPS9
Massive capability structure returned by `GetDeviceCaps()`. Key fields:
- `DevCaps` — device capabilities flags
- `MaxTextureWidth/Height` — max texture dimensions
- `MaxSimultaneousTextures` — max bound textures
- `MaxStreams` — max vertex streams
- `VertexShaderVersion` — e.g., D3DVS_VERSION(3,0)
- `PixelShaderVersion` — e.g., D3DPS_VERSION(3,0)
- `NumSimultaneousRTs` — MRT support count
- `MaxVertexShaderConst` — vertex shader constant register count

## Swap Chains and Presentation

A swap chain (`IDirect3DSwapChain9`) manages back buffers for frame presentation. The implicit swap chain is created with the device; additional swap chains support multiple viewports or windows.

Render loop pattern:
```
device->BeginScene();
  // Set states, bind resources, draw primitives
device->EndScene();
device->Present(NULL, NULL, NULL, NULL);  // flip/copy back buffer to front
```

`Present()` parameters: source rect, dest rect, override window, dirty region. Most games pass all NULL.

Lost devices: When the device is lost (Alt-Tab in fullscreen, display mode change), `Present()` returns `D3DERR_DEVICELOST`. The app must call `TestCooperativeLevel()` in a loop, then `Reset()` the device after releasing all D3DPOOL_DEFAULT resources. `D3DPOOL_MANAGED` resources survive `Reset()`.

## Resource Management

### Memory Pools (D3DPOOL)
- `D3DPOOL_DEFAULT` — video memory (fastest, lost on Reset)
- `D3DPOOL_MANAGED` — system + video memory (runtime manages caching, survives Reset)
- `D3DPOOL_SYSTEMMEM` — system memory only (cannot be render target)
- `D3DPOOL_SCRATCH` — system memory, no GPU access at all

### Usage Flags (D3DUSAGE)
- `D3DUSAGE_DYNAMIC` — frequently updated by CPU (uses write-combined memory)
- `D3DUSAGE_WRITEONLY` — CPU will only write, not read (enables optimization)
- `D3DUSAGE_RENDERTARGET` — surface can be a render target
- `D3DUSAGE_DEPTHSTENCIL` — surface is a depth/stencil buffer
- `D3DUSAGE_AUTOGENMIPMAP` — runtime auto-generates mipmaps

### Locking Resources
`Lock()` / `Unlock()` on buffers and surfaces for CPU access. Critical performance rules:
- Lock with `D3DLOCK_DISCARD` when rewriting entire dynamic buffer (avoids stalls)
- Lock with `D3DLOCK_NOOVERWRITE` when appending to dynamic buffer
- Never lock default-pool static resources during rendering
- Prefer `UpdateTexture()` / `UpdateSurface()` for managed→default transfers

## Coordinate Systems and Transforms

D3D9 uses a left-handed coordinate system by default (positive Z goes into the screen).

### Transform Pipeline (Fixed-Function)
Three matrix stages set via `SetTransform()`:
1. **World Transform** (`D3DTS_WORLD` / `D3DTS_WORLDMATRIX(n)`) — object space → world space
2. **View Transform** (`D3DTS_VIEW`) — world space → camera space. Typically built with `D3DXMatrixLookAtLH()`.
3. **Projection Transform** (`D3DTS_PROJECTION`) — camera space → clip space. Built with `D3DXMatrixPerspectiveFovLH()` or `D3DXMatrixOrthoLH()`.

After projection: hardware performs homogeneous divide (perspective divide) → NDC → viewport transform → screen coordinates.

### Texture Transforms
`D3DTS_TEXTURE0` through `D3DTS_TEXTURE7` transform texture coordinates. Controlled via `D3DTSS_TEXTURETRANSFORMFLAGS`.

## Viewports and Clipping

The viewport (`D3DVIEWPORT9`) maps NDC to screen pixels:
- `X`, `Y` — top-left of viewport in pixels
- `Width`, `Height` — viewport dimensions in pixels
- `MinZ`, `MaxZ` — depth range mapping (typically 0.0–1.0)

Set via `SetViewport()`. Multiple viewports enable split-screen or picture-in-picture by changing the viewport between draw calls.

Clipping: D3D9 clips primitives to the view frustum automatically. User clip planes (`SetClipPlane()`, up to `MaxUserClipPlanes` from caps) add custom clipping. Guard-band clipping extends the clip region beyond the viewport for efficiency (`GuardBandLeft/Right/Top/Bottom` in caps).

---

## Key Microsoft Docs URLs

- Architecture: `https://learn.microsoft.com/en-us/windows/win32/direct3d9/direct3d-architecture`
- Devices: `https://learn.microsoft.com/en-us/windows/win32/direct3d9/direct3d-devices`
- Resources: `https://learn.microsoft.com/en-us/windows/win32/direct3d9/direct3d-resources`
- Transforms: `https://learn.microsoft.com/en-us/windows/win32/direct3d9/transforms`
- Viewports: `https://learn.microsoft.com/en-us/windows/win32/direct3d9/viewports-and-clipping`
- Surfaces: `https://learn.microsoft.com/en-us/windows/win32/direct3d9/direct3d-surfaces`
