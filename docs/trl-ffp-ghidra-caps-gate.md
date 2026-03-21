# TRL.exe: D3D9 Caps Gate → Force Legacy FFP (Verified Offsets)

Date: 2026-01-31

## Goal
Identify the exact D3D9 hardware capability gate in `trl.exe` that enables the “Next‑Gen Content” render path, then force the legacy Fixed Function Pipeline (FFP) fallback required for NVIDIA RTX Remix compatibility.

This note focuses on the *caps* side of the gate: where `D3DCAPS9` is populated and where `PixelShaderVersion` / `VertexShaderVersion` are consumed.

## Reality Check (What’s Missing)
This repo does **not** contain `trl.exe`, so I can’t currently produce the concrete function addresses / decompiler screenshots from Ghidra. The sections below are the exact, reproducible search steps + the verified struct/vtable offsets you can use once the binary is available locally.

## Expected Call Graph (D3D9 Init → Capability Gate)
Typical D3D9-era flow (names will differ in `trl.exe`, but the call shape is consistent):

- engine bootstrap / config load
  - loads “Next‑Gen Content” user toggle (or equivalent)
- graphics init
  - `Direct3DCreate9(D3D_SDK_VERSION)`
  - adapter/mode enumeration (commonly: `GetAdapterIdentifier`, `GetAdapterDisplayMode`, `EnumAdapterModes`)
  - **capability detection**
    - `IDirect3D9::GetDeviceCaps(Adapter, DevType, &caps)`
    - hardware gate reads `caps.PixelShaderVersion` / `caps.VertexShaderVersion`
    - sets boolean(s): `supportsSM20`, `supportsSM30`, `nextGenCapable`, etc.
  - path select
    - `if (NextGenEnabledByUser && NextGenSupportedByHardware) { nextGenPath(); } else { legacyFFPPath(); }`
  - device creation
    - `IDirect3D9::CreateDevice(...)`
  - post-create device setup (render states, textures, shaders/FFP state)

## D3DCAPS9 Field Offsets (Verified)
Offsets below are from the official Direct3D9 headers (structure is all 4-byte fields up to and including these members, so no padding surprises in practice).

- **`D3DCAPS9::VertexShaderVersion`**: `0xC4` (196)
- **`D3DCAPS9::PixelShaderVersion`**: `0xCC` (204)

Verification source used in this environment:
- `d3d9caps.h` (`/usr/share/mingw-w64/include/d3d9caps.h`) shows the exact member order.
- A `ctypes.Structure` recreation confirmed the offsets at runtime.

### Shader Version Constants You’ll See in Code
Direct3D9 encodes shader versions as:

- `D3DVS_VERSION(2,0)` = `0xFFFE0200`
- `D3DVS_VERSION(3,0)` = `0xFFFE0300`
- `D3DPS_VERSION(2,0)` = `0xFFFF0200`
- `D3DPS_VERSION(3,0)` = `0xFFFF0300`

So in `trl.exe` the “supports PS2/PS3” checks are often simple integer comparisons against those DWORDs (or extracting major/minor).

## IDirect3D9 VTable Indices (For Ghidra + Hooking)
`IDirect3D9` methods are invoked via vtable calls (not direct imports), so the easiest signature is the vtable *slot*:

- **`IDirect3D9::GetDeviceCaps`**: vtable index **14** → vtable offset **`0x38`** (`14 * 4`)
- **`IDirect3D9::CreateDevice`**: vtable index **16** → vtable offset **`0x40`** (`16 * 4`)

This matters in Ghidra because you’ll see patterns like “load vtable pointer” then `call [vtable + 0x38]` / `call [vtable + 0x40]`.

## Ghidra Discovery Recipe (No Guesswork)

### 1) Find the D3D9 bootstrap
- **Imports**: search for `Direct3DCreate9` in the import table.
- **Xrefs**: follow cross-references to the wrapper/init function that calls it.

Expected result: a function that returns/stores an `IDirect3D9*` and then starts adapter/mode selection.

### 2) Find where `D3DCAPS9` is populated
You’re looking for `IDirect3D9::GetDeviceCaps(Adapter, DevType, &caps)`.

In x86 code, the most reliable pattern is:
- a local stack buffer (or global) passed by pointer,
- then a vtable call at offset `0x38`.

Once you find the callsite:
- identify the `D3DCAPS9` buffer address (`&caps`),
- track where the buffer is later read.

### 3) Find the shader-model gate (the “boolean flag”)
After the `GetDeviceCaps` call, look for reads at:
- `caps + 0xC4` → `VertexShaderVersion`
- `caps + 0xCC` → `PixelShaderVersion`

Then identify the branch that sets the “Next‑Gen capable” flag. Common shapes:
- **SM3 path**:
  - `if (PS >= 0xFFFF0300 && VS >= 0xFFFE0300) nextGenCapable = true;`
- **SM2 vs SM3 split**:
  - `if (PS >= 0xFFFF0200 && VS >= 0xFFFE0200) { enableShaders = true; }`
  - `if (PS >= 0xFFFF0300 && VS >= 0xFFFE0300) { enableNextGen = true; }`

Practical search tips:
- search for immediate constants `0xFFFF0300`, `0xFFFE0300`, `0xFFFF0200`, `0xFFFE0200`
- search for compares against `0x300` / `0x200` too (some code uses `MAJOR(version)` extraction)

### 4) Cross-reference with the “Next‑Gen Content” toggle
Once you find the gate, trace back to:
- config parsing / dvar / registry / ini reads,
- menu toggle handlers,
- whichever global “graphics profile” structure gets updated.

What you want is the combined condition:
`NextGenEnabledByUser && NextGenSupportedByHardware`

The hardware side is what we’ll spoof.

## Implementation Plan: dinput8.dll Proxy + MinHook (Spoof Caps)
Objective: make the engine believe shaders are unsupported so it selects the legacy/FFP renderer.

### Hook target
Intercept `IDirect3D9::GetDeviceCaps` and post-process the filled `D3DCAPS9`:

- `caps.VertexShaderVersion = 0;`
- `caps.PixelShaderVersion = 0;`

This forces any “requires SM2/SM3” checks to fail.

### Injection method
Use a `dinput8.dll` proxy placed next to `trl.exe`:
- loads the real system `dinput8.dll`,
- forwards required exports (at minimum `DirectInput8Create`),
- initializes MinHook early.

### Hook install strategy (robust ordering)
- Hook `Direct3DCreate9` (by name) when `d3d9.dll` is present.
- In the `Direct3DCreate9` hook:
  - call original to get the real `IDirect3D9*`,
  - hook the vtable slot 14 (`GetDeviceCaps`) for that interface instance.

Why: `CreateDevice` is too late if the engine chooses the renderer profile *before* device creation.

### Minimal pseudocode shape (what you’ll actually implement)
- `dinput8.dll`:
  - `LoadLibraryW(<system>\\dinput8.dll)`
  - `GetProcAddress` for the exports you forward (at minimum `DirectInput8Create`)
  - spin up an init thread that:
    - `MH_Initialize()`
    - `MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9", ...)`
    - `MH_EnableHook(MH_ALL_HOOKS)`
- `Direct3DCreate9` hook:
  - call original, then:
    - `void** vtbl = *(void***)(pD3D9);`
    - `void* getDeviceCaps = vtbl[14];`
    - `MH_CreateHook(getDeviceCaps, GetDeviceCaps_Hook, &GetDeviceCaps_Orig)`
- `GetDeviceCaps` hook:
  - call original, then:
    - `pCaps->VertexShaderVersion = 0;`
    - `pCaps->PixelShaderVersion = 0;`

### Safety notes
- only spoof caps for the target process/module (`trl.exe`)
- log the *original* shader versions once for validation (expect RTX-era GPUs to report PS/VS `0xFFFF0300` / `0xFFFE0300`)

