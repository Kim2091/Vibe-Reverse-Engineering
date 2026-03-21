# TRL FFP Discovery Note

Date: 2026-01-31

## Scope
Deep-dive analysis target: trl.exe (Tomb Raider: Legend) for forcing Fixed Function Pipeline (FFP) for RTX Remix.

## Findings (Verified with Ghidra)

### 1. D3D9 Initialization
- **Function**: `00ec74xx` (approx)
- **Global IDirect3D9* Variable**: `01392e18`
- **Dynamic Loading**: Uses `LoadLibraryA("d3d9.dll")` and `GetProcAddress("Direct3DCreate9")` manually.

### 2. Capability Gate (Next-Gen Check)
- **Function Address**: `00ec2d10`
- **Logic**: Checks if hardware supports Shader Model 3.0.
- **Decompiled Logic**:
  ```c
  bool IsNextGenCapable(Renderer* this) {
      // Offsets relative to Renderer object (ECX)
      // D3DCAPS9 struct starts at offset 0x5B8
      DWORD vsVersion = this->caps.VertexShaderVersion; // Offset 0x67C (0x5B8 + 0xC4)
      DWORD psVersion = this->caps.PixelShaderVersion; // Offset 0x680 (0x5B8 + 0xC8)

      if (vsVersion >= D3DVS_VERSION(3,0) && psVersion >= D3DPS_VERSION(3,0)) {
          return true;
      }
      return false;
  }
  ```
- **Constants Used**:
  - `0xFFFE0300` (VS 3.0)
  - `0xFFFF0300` (PS 3.0)

### 3. GetDeviceCaps Wrapper
- **Function Address**: `00ecd480`
- **Behavior**: Calls `IDirect3D9::GetDeviceCaps` (vtable offset 0x38) and stores the result in the `Renderer` object's caps structure.

## Hook Strategy (Confirmed)

To force the Legacy (FFP) path, we must ensure `IsNextGenCapable` returns `false`.
Since this function reads from the cached `D3DCAPS9` structure, we should hook `IDirect3D9::GetDeviceCaps` and zero out the shader versions in the returned structure.

### Hook Plan
1.  **Hook `Direct3DCreate9`**:
    - Intercept the creation to get the `IDirect3D9` interface pointer.
2.  **Hook `GetDeviceCaps` (VTable Hook)**:
    - Hook the function at index 14 (offset 0x38) of the `IDirect3D9` vtable.
3.  **In `GetDeviceCaps_Hook`**:
    - Call original.
    - Modify `pCaps->VertexShaderVersion = 0`.
    - Modify `pCaps->PixelShaderVersion = 0`.
    - (Optional) Force `DevType` to `D3DDEVTYPE_HAL` if needed, but usually just caps is enough.

This will cause the engine's internal copy of caps to have 0 version, failing the check at `00ec2d10`.

## Next Steps
- Implement the hook using MinHook and a dinput8.dll proxy.
- Verify in-game that "Next-Gen Content" is disabled/greyed out or effectively off.
