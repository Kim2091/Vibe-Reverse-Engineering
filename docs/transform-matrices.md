# Transform Matrices in RTX Remix

## The Transform Pipeline

In standard D3D9 FFP, vertices are transformed through:

```
Object Space → [World Matrix] → World Space → [View Matrix] → View Space → [Projection] → Clip Space
```

Remix needs to reconstruct **world-space positions** for ray tracing. It does this by:

1. Reading the vertex data (assumed object-space or world-space)
2. Reading `SetTransform(D3DTS_WORLD)`, `SetTransform(D3DTS_VIEW)`, `SetTransform(D3DTS_PROJECTION)`
3. Applying the World matrix to place geometry in world space
4. Using View + Projection for camera setup

## fusedWorldViewMode

This is the **most critical setting** for getting geometry in the right place.

```ini
rtx.fusedWorldViewMode = 0 | 1 | 2
```

### Mode 0: None
- Remix takes World, View, Projection matrices as-is
- Assumes the game properly sets all three via SetTransform
- **Use when**: Game sets proper separate W, V, P matrices

### Mode 1: World-View Fused
- Remix assumes `SetTransform(D3DTS_WORLD)` actually contains `World × View`
- Remix will try to extract the View component using the camera info
- **Use when**: Game pre-multiplies World and View together

### Mode 2: World-View-Projection Fused (USUALLY WRONG — DON'T START HERE)
- Remix assumes `SetTransform(D3DTS_WORLD)` contains the full `World × View × Projection`
- Very aggressive decomposition, often produces artifacts
- **Use when**: Nothing else works and geometry is otherwise invisible

### For TRL Proxy

Since you control what SetTransform receives, **use Mode 0**:

```cpp
// In your proxy, after decomposing WVP from shader constants:
device->SetTransform(D3DTS_WORLD, &worldMatrix);       // Object → World
device->SetTransform(D3DTS_VIEW, &viewMatrix);          // World → View
device->SetTransform(D3DTS_PROJECTION, &projMatrix);    // View → Clip
```

And in rtx.conf:
```ini
rtx.fusedWorldViewMode = 0
```

This gives Remix clean, separate matrices and avoids decomposition errors.

## Matrix Decomposition Strategies for TRL

### Strategy 1: Intercept SetTransform + Shader Constants

TRL may still call `SetTransform(D3DTS_VIEW, ...)` and `SetTransform(D3DTS_PROJECTION, ...)`
even though it uses shaders. Capture these:

```cpp
HRESULT SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    if (State == D3DTS_VIEW) {
        m_viewMatrix = *pMatrix;
    } else if (State == D3DTS_PROJECTION) {
        m_projMatrix = *pMatrix;
    } else if (State == D3DTS_WORLD) {
        m_worldMatrix = *pMatrix;
    }
    return m_realDevice->SetTransform(State, pMatrix);
}
```

Then for shader-based draws, recover World from WVP:
```cpp
D3DXMATRIX vp = m_viewMatrix * m_projMatrix;
D3DXMATRIX vpInv;
D3DXMatrixInverse(&vpInv, NULL, &vp);
D3DXMATRIX recoveredWorld = m_wvpMatrix * vpInv;
```

### Strategy 2: Reverse-Engineer Camera from Game Memory

Use a tool like Cheat Engine or IDA to find TRL's camera position/rotation in memory.
Then reconstruct the View matrix:

```cpp
D3DXMATRIX BuildViewMatrix(D3DXVECTOR3 eye, D3DXVECTOR3 target, D3DXVECTOR3 up) {
    D3DXMATRIX view;
    D3DXMatrixLookAtLH(&view, &eye, &target, &up);
    return view;
}
```

This is more reliable than decomposing WVP because you get exact camera parameters.

### Strategy 3: Static World Matrix for Level Geometry

If level geometry vertices are already in world space (common for BSP/level data):

```cpp
D3DXMATRIX identity;
D3DXMatrixIdentity(&identity);
device->SetTransform(D3DTS_WORLD, &identity);
```

Only movable objects (Lara, enemies, props) need non-identity World matrices.

## Vertex Capture and Transform Reversal

When `rtx.useVertexCapture = True`, Remix reads post-VS output (clip-space positions) and
attempts to reverse them back to world space using the SetTransform matrices.

The math Remix does internally:
```
worldPos = clipPos × inverse(View × Projection)
```

This ONLY works if:
1. `SetTransform(D3DTS_VIEW)` matches the actual View the shader used
2. `SetTransform(D3DTS_PROJECTION)` matches the actual Projection the shader used
3. The shader's transform is purely `pos × WVP` (no per-vertex animation/deformation)

If TRL's shaders do any non-standard vertex manipulation (procedural animation, vertex displacement),
vertex capture will produce wrong world positions.

## Common Pitfalls

### 1. Identity View Matrix from DXWrapper

If using dxwrapper/d3d8to9, it may set `SetTransform(D3DTS_VIEW, &identity)`. This means Remix
thinks the camera is at origin looking down +Z, and all geometry appears at the wrong position.

**Fix**: Override View matrix in your proxy with the actual camera transform.

### 2. Z-Up vs Y-Up

Remix default is Y-up. TRL's engine might use Y-up or Z-up depending on the coordinate system.

```ini
rtx.zUp = False    # Y-up (default, most D3D9 games)
rtx.zUp = True     # Z-up (some engines)
```

Check by looking at geometry in the Remix toolkit capture viewer — if everything is sideways, flip this.

### 3. Near/Far Plane Issues

TRL may use extreme near/far ratios that cause precision issues in Remix's matrix operations.

```ini
rtx.nearPlaneOverride = 0.1    # Override near plane for better precision
```

### 4. Pre-Transformed Vertices (RHW)

If TRL ever submits pre-transformed vertices (`D3DFVF_XYZRHW` or `D3DDECLUSAGE_POSITIONT`),
these are already in screen space. Remix cannot ray-trace screen-space geometry.

**Fix**: These are almost always UI/HUD. Skip them in FFP conversion and tag as UI textures.

## Validation Checklist

After setting up transforms, verify:

- [ ] Geometry appears in roughly correct positions in Remix debug view
- [ ] Geometry doesn't move/shift when camera pans (would indicate camera baked into World)
- [ ] Geometry Hash debug view shows stable colors (no flickering)
- [ ] Captured USD scenes in Remix Toolkit show geometry at correct world positions
- [ ] Near objects don't clip or distort
- [ ] Skybox is at appropriate distance (not at origin)
