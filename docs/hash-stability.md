# Geometry Hash Stability in RTX Remix

## How Remix Computes Geometry Hashes

RTX Remix identifies each piece of geometry by hashing its **vertex buffer contents** combined with
its **index buffer** and **bound texture hashes**. The hash is what Remix uses to:

- Apply material replacements
- Anchor light placements
- Track instances across frames
- Apply asset replacements

**Critical insight**: The hash is computed from the **raw vertex data as seen by Remix**. If ANY
part of the vertex data changes between frames, the hash changes, and all replacements/lights are lost.

## What Causes Hash Instability

### 1. Camera-Dependent World Matrix Baked into Vertices

**THE #1 CAUSE for shader-based games like TRL.**

If your proxy DLL multiplies vertices by the full WVP matrix before submitting to Remix, the vertex
positions change every time the camera moves. This means a different hash every frame.

**Fix**: Submit vertices in object/model space. Set the World matrix via `SetTransform(D3DTS_WORLD)`.
Let Remix handle the View/Projection internally.

### 2. Software Skinning (CPU-side bone transforms)

If TRL applies bone transforms on the CPU before submitting vertex data, each animation frame
produces different vertex positions → different hash.

**Fix**: For skinned meshes, accept that hashes will be dynamic. Use `rtx.useVertexCapture = True`
and ensure Remix's instance tracking (`rtx.calculateAxisAlignedBoundingBox = True`) can follow them.
Skinned mesh hashes are inherently unstable — Remix has mechanisms to track them, but they can't be
replaced via static hash matching.

### 3. Vertex Buffer Reuse / Dynamic VBs

Games often reuse the same vertex buffer for different geometry, filling it with new data each draw.
If your proxy creates vertex buffers dynamically, the buffer address and contents change.

**Fix**: Cache converted vertex buffers keyed by a stable identifier (e.g., the original VB pointer +
offset + size). Only regenerate when the source data actually changes.

### 4. Floating Point Precision / SHORT4 Conversion Drift

If SHORT4→FLOAT3 conversion uses slightly different precision each frame (e.g., different scale
factors), the resulting floats drift and the hash changes.

**Fix**: Use exactly the same conversion formula every time. Cache the converted output if possible.
Consider using `rtx.geometryHashGenerationRoundPosTo` to round positions to reduce precision noise.

### 5. Dynamic Geometry (particles, water, cloth)

These legitimately change every frame. They will never have stable hashes.

**Fix**: Tag them appropriately in rtx.conf:
- Particles → `rtx.particleTextures`
- Dynamic/animated → accept instability, use anti-culling to keep them visible

## Debugging Hash Stability

### Visual Debug

1. Launch game with Remix
2. Press `Alt+X` → Developer Settings → Debug tab
3. Set Debug View → **Geometry Hash**
4. Each mesh gets a unique color based on its hash
5. **Stable**: Colors don't change as camera moves
6. **Unstable**: Colors flicker/shift = hash changing every frame

### Automated Hash Logging

In rtx.conf:
```
rtx.enableDebugMode = True
```

This enables hash overlays when combined with the debug view. You can also enable hash collision
detection with `rtx.hashCollisionDetection = True`.

## Hash-Related rtx.conf Settings

```ini
# Geometry hashing
rtx.geometryHashGenerationRoundPosTo = 0.01
# Round vertex positions to reduce floating-point noise
# Higher values = more forgiving but may merge distinct meshes

rtx.geometryHashRoundTextureCoordinatesTo = 0.0001
# Round UVs similarly

rtx.calculateAxisAlignedBoundingBox = True  
# Compute AABB per draw call for better instance tracking
# Helps with skinned/vertex-shaded meshes

# Vertex capture (alternative to FFP)
rtx.useVertexCapture = True
# Intercepts post-VS output and uses SetTransform to reverse-map
# Can help with shader-based games but requires correct transforms

# Hash compatibility
rtx.useXXH64ForTextures = False
# Use newer hash algorithm (don't enable for new projects)

# Debug
rtx.hashCollisionDetection = True
rtx.enableDebugMode = True
```

## Vertex Capture Mode vs FFP Proxy

There are two approaches for shader-based games:

### FFP Proxy (Recommended for TRL)
- Null shaders, re-emit as FFP
- Full control over vertex data
- Stable hashes if done correctly
- More work but more reliable

### Vertex Capture (`rtx.useVertexCapture = True`)
- Remix intercepts post-vertex-shader output
- Uses SetTransform matrices to reverse-map from clip space to world space
- Less proxy code needed
- **BUT**: If the game's SetTransform matrices are wrong (identity, or not matching the shader's
  actual transform), Remix can't reverse-map correctly → geometry ends up in wrong position
- Hash stability depends on vertex shader output consistency

**For TRL, FFP proxy is recommended** because:
- Full control over what Remix sees
- Can guarantee stable vertex data
- Can properly decompose transforms
- Vertex capture would require TRL's SetTransform to match its shader transforms (unlikely without modification)

## Anchor Assets for Unstable World Geometry

When world geometry has inherently unstable hashes (e.g., because the game rebuilds vertex buffers
each frame for culled/visible sets), use **Anchor Assets**:

1. Find a mesh in the scene with a **stable hash** (e.g., a prop that never changes)
2. In the RTX Remix Toolkit, attach replacement assets relative to that anchor
3. The anchor's stable hash acts as a reference point
4. Even if surrounding geometry hashes change, the anchor-relative placement stays fixed

This is NVIDIA's official workaround for games with aggressive visibility/culling systems.

## TRL-Specific Hash Strategy

1. **World geometry**: Should have stable hashes if proxy correctly keeps vertices in object space
   with a proper World matrix. Each room/section of geometry gets a consistent hash.

2. **Lara (player model)**: Skinned mesh — hashes will be dynamic. This is expected and OK.
   Remix tracks skinned meshes via instance tracking, not static hash matching.

3. **Props/pickups**: Should be stable if they use static vertex buffers.

4. **Particles/effects**: Inherently unstable. Tag as particles in rtx.conf.

5. **Skybox**: Should be stable. Tag as sky texture if needed.
