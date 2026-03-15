# Skinning Extension — FFP Indexed Vertex Blending

## When to Enable

Enable skinning **only after** basic FFP conversion works:

- [ ] Rigid geometry (world, props) renders correctly with Remix
- [ ] VS constant register mapping confirmed (View/Proj/World)
- [ ] AlbedoStage set correctly in `proxy.ini`
- [ ] HUD/UI pass-through working (not garbled or misplaced)

Skinning is for **character models and animated meshes** that use BLENDWEIGHT + BLENDINDICES vertex elements with bone matrices in VS constant registers.

## How to Enable

1. In `d3d9_device.c`, change:
   ```c
   #define ENABLE_SKINNING 0
   ```
   to:
   ```c
   #define ENABLE_SKINNING 1
   ```

2. Verify the bone register defines match the game:
   ```c
   #define VS_REG_BONE_THRESHOLD  20   // First register that could be a bone
   #define VS_REGS_PER_BONE        3   // Registers per bone (3 = packed 4x3)
   #define VS_BONE_MIN_REGS        3   // Minimum count to trigger detection
   ```

3. Rebuild with `build.bat`.

## What It Does

When enabled, `d3d9_skinning.h` is included and provides:

| Function | Purpose |
|----------|---------|
| `Skin_DrawDIP()` | Handles the entire skinned draw path: expand vertices → upload bones → FFP draw |
| `Skin_InitDevice()` | Creates the shared expansion vertex declaration at device creation |
| `Skin_ReleaseDevice()` | Frees VB cache and expansion declaration on device release |
| `SkinVB_GetExpanded()` | Expands source vertices to a fixed 48-byte FLOAT layout with caching |
| `FFP_UploadBones()` | Uploads bone matrices as D3DTS_WORLDMATRIX(n) for indexed vertex blending |
| `FFP_DisableSkinning()` | Resets indexed vertex blending state between skinned and rigid draws |

### Vertex Expansion

Source vertices use compressed formats (SHORT4N normals, FLOAT16_2 UVs, UBYTE4N weights) that FFP and Remix cannot consume directly. The expansion step decodes every vertex into:

```
offset  0: FLOAT3 POSITION        (12 bytes)
offset 12: FLOAT3 BLENDWEIGHT     (12 bytes, unused slots = 0.0)
offset 24: UBYTE4 BLENDINDICES    ( 4 bytes)
offset 28: FLOAT3 NORMAL          (12 bytes, decoded from any source format)
offset 40: FLOAT2 TEXCOORD[0]     ( 8 bytes)
total:     48 bytes per vertex
```

Expanded VBs are cached by a hash of (source VB pointer, base vertex, count, stride, declaration pointer) to avoid re-expansion on every draw.

### Bone Upload

Bone matrices are detected by writes to `SetVertexShaderConstantF` at registers ≥ `VS_REG_BONE_THRESHOLD` with count divisible by `VS_REGS_PER_BONE`. Each bone is a 4x3 packed matrix (3 vec4 registers) that gets transposed and padded to 4x4 for `SetTransform(D3DTS_WORLDMATRIX(n))`.

FFP indexed vertex blending mode is set to `D3DVBF_1WEIGHTS`, `D3DVBF_2WEIGHTS`, or `D3DVBF_3WEIGHTS` based on the BLENDWEIGHT element type.

## RE Investigation for Bone Registers

Before enabling, confirm the bone register layout with RE tools:

```bash
# Find SetVertexShaderConstantF call sites
python -m retools.xrefs game.exe <IAT_addr_of_SetVSConstF> -t call

# Decompile a call site to see register ranges
python -m retools.decompiler game.exe <call_site_addr> --types patches/<game>/kb.h

# Live trace to see actual register values
python -m livetools trace <SetVSConstF_wrapper> --count 50 \
  --read "[esp+4]:4:uint32; [esp+8]:4:uint32"
# → startReg and count. Bone writes are large (count=48+ registers in one call)
```

Look for patterns like: startReg=20+, count divisible by 3, written once per character per frame.

## Limitations

- FFP indexed vertex blending supports ~48 bones max (driver-dependent)
- GPU-side skinning quality matches the D3D9 FFP implementation, not the original shader
- Single UV channel (TEXCOORD[0] only) in the expansion layout
- No tangent/binormal preservation (normal mapping won't carry through)

## Troubleshooting

| Symptom | Likely Cause |
|---------|-------------|
| Characters at origin (0,0,0) | Wrong `VS_REG_BONE_THRESHOLD` — bones not detected |
| T-pose / bind pose | Bone register writes are happening but `VS_REGS_PER_BONE` is wrong |
| Garbled vertices | Source vertex stride or element offsets wrong — check `SetVertexDeclaration` parsing |
| Characters black/white | Albedo texture not routed — check `AlbedoStage` in `proxy.ini` |
| Rigid meshes distorted after character draw | `worldDirty` flag not triggering — `FFP_DisableSkinning` may not be called |
