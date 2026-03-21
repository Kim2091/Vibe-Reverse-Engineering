# Troubleshooting Guide — TRL RTX Remix

## Crash / Won't Start

### Game crashes on launch with Remix
- **Check**: Is Remix d3d9.dll 64-bit and Bridge present? TRL is 32-bit, needs Bridge.
- **Check**: Is `NvRemixBridge.exe` in the game directory?
- **Check**: Is `.trex/` folder present with bridge config?
- **Try**: Run with minimal rtx.conf (just `rtx.enableRaytracing = True`)
- **Try**: Disable "Next Generation Content" in TRL settings
- **Try**: Set `dxvk.enableAftermath = True` in dxvk.conf and check crash dumps

### Game crashes when proxy DLL loads
- **Check**: Is proxy compiled as 32-bit (x86)?
- **Check**: Does proxy correctly load d3d9_remix.dll?
- **Check**: LoadLibrary error code (use GetLastError after LoadLibrary)
- **Try**: Add debug MessageBox at DllMain to verify it's loading

### Remix splash message appears but game freezes
- **Check**: Is the proxy forwarding ALL D3D9 calls? Missing any → hang
- **Check**: IUnknown methods (QueryInterface, AddRef, Release) must be forwarded
- **Check**: Present() must be forwarded

## No Geometry Visible

### Remix loads but Geometry Hash view is empty
- **Cause**: All draw calls use shaders, no FFP calls reaching Remix
- **Fix**: Implement FFP conversion in proxy (null shaders before draw calls)
- **Verify**: Log that your proxy is actually calling DrawIndexedPrimitive after nulling shaders

### Geometry Hash view shows geometry but it's in the wrong place
- **Cause**: Transform matrices are incorrect
- **Check fusedWorldViewMode**: Try 0, 1, 2 to see which places geometry correctly
- **Check**: Log World/View/Proj matrices and verify they look reasonable
- **Check**: Is World matrix identity when it shouldn't be? (or vice versa)
- **Check**: Coordinate system — try `rtx.zUp = True` if everything is sideways

### Some geometry visible, some missing
- **Cause**: Proxy only converts some draw calls, or vertex format issues
- **Check**: Are you filtering out draw calls you shouldn't be?
- **Check**: Do missing meshes use a different vertex format?
- **Check**: SHORT4 positions not being converted to FLOAT3?
- **Check**: Different shader programs for different geometry types?

## Hash Instability

### All geometry hashes flicker every frame
- **Root cause**: Camera transform baked into vertex data or World matrix
- **Quick test**: Stand still, don't move camera. Do hashes still flicker?
  - Yes → vertex buffer data changes even when nothing moves (dynamic VBs, animation)
  - No → camera is leaking into data. Fix matrix decomposition.

### Hashes stable when standing still, flicker when camera moves
- **Root cause**: View matrix component in World matrix
- **Fix**: Verify SetTransform(D3DTS_WORLD) doesn't include camera rotation
- **Fix**: Ensure View matrix is correctly separated

### Hashes stable for most geometry, a few objects flicker
- **Likely cause**: Those objects use dynamic vertex buffers or software skinning
- **For skinned meshes**: Expected — enable `rtx.calculateAxisAlignedBoundingBox = True`
- **For world geometry**: Check if those specific draw calls use a different shader
  or a different vertex buffer that your proxy handles differently

### Hashes change when returning to an area
- **Cause**: Game rebuilds vertex buffers with slightly different data
- **Fix**: Increase `rtx.geometryHashGenerationRoundPosTo` (try 0.01, 0.1)
- **Fix**: Cache converted vertex data keyed by source VB identity

## Light Placement Issues

### Light placed in Toolkit doesn't appear in game
- **Check**: Is mod layer in `rtx-remix/mods/` directory?
- **Check**: Is `rtx.baseGameModPath` set correctly in rtx.conf?
- **Check**: Is the geometry hash the light references still valid?
- **Try**: Recapture scene and re-place light

### Light appears but moves when camera rotates
- **Root cause**: World matrix includes View transform
- **Proof**: If the light appears to orbit around the mesh when you rotate the camera,
  the mesh's world position is camera-dependent
- **Fix**: Fix matrix decomposition. World must be pure object-to-world.

### Light appears but moves when Lara walks
- **Root cause**: World matrix includes some player-position component,
  OR the geometry hash is unstable and Remix is matching a different mesh
- **Check**: Geometry Hash debug — is the target mesh's hash stable?
- **Check**: Does the mesh's apparent world position shift when Lara moves?

### Light is in the right place but too dim / invisible
- **Check**: Light intensity values (Remix uses physically-based units)
- **Try**: Increase luminosity by 10x as a test
- **Check**: Is the light inside geometry? (won't illuminate surfaces it's inside)
- **Check**: Is the light type correct? (Distant lights illuminate everywhere,
  Sphere lights have limited range)

### D3D9 lights from proxy not picked up by Remix
- **Check**: Are you calling `SetLight()` + `LightEnable()` on the real device?
- **Check**: Is `rtx.ignoreGamePointLights` or similar set to True?
- **Check**: Is the light position in world space? (not view space)
- **Check**: Are the light parameters reasonable? (Range > 0, Diffuse not zero)

## Performance Issues

### Very low FPS with path tracing
- **Check**: DLSS enabled? (`rtx.qualityDLSS = 3` for Performance)
- **Check**: Anti-culling too aggressive? Reduce lifetime and fovScale
- **Check**: Too many draw calls? Log draw call count per frame
- **Check**: Are you accidentally submitting the same geometry multiple times?
- **Check**: Are screen-space quads being ray-traced? (should be filtered)

### Stuttering / Hitching
- **Cause**: Shader compilation (first-time in new areas)
- **Cause**: Vertex buffer conversion overhead in proxy
- **Fix**: Cache all conversions
- **Fix**: Pre-warm by walking through the level once

## Visual Artifacts

### Wireframe / grid lines on geometry
- **Cause**: Mesh edge artifacts in path tracing, often from thin triangles
- **Try**: `rtx.enableSmoothNormals = True`
- **Try**: Adjust denoiser settings

### Z-fighting / flickering surfaces
- **Cause**: Two meshes at nearly the same position
- **Try**: Adjust near plane: `rtx.nearPlaneOverride = 0.1`
- **Try**: Enable decal handling for overlapping surfaces

### Black geometry / no textures
- **Cause**: Texture not bound or texture hash not recognized
- **Check**: Is SetTexture(0, ...) called before the FFP draw?
- **Check**: Is the texture format supported by Remix?
- **Check**: Are multi-texture stages set up correctly?

### Geometry exploding / vertices flying everywhere  
- **Cause**: Vertex format mismatch or incorrect conversion
- **Check**: SHORT4 scale factor — are positions being scaled correctly?
- **Check**: Vertex stride matches between original and converted buffer
- **Check**: Index buffer values within range of vertex buffer

### Sky rendered as a small box nearby
- **Cause**: Sky geometry's World matrix places it at camera distance, not infinity
- **Fix**: Tag sky textures: `rtx.skyBoxTextures = HASH`
- **Fix**: Or filter sky draw calls out of FFP conversion

## Debug Workflow

When stuck, follow this systematic approach:

1. **Simplify**: Comment out proxy conversion, verify game runs
2. **Add one thing**: Enable FFP conversion for ONE draw call only
3. **Verify**: Does that one mesh appear in Geometry Hash view?
4. **Expand**: Gradually add more draw calls
5. **Identify**: Which draw call type causes the problem?
6. **Isolate**: What's different about that draw call? (shader, VB format, textures)
7. **Fix**: Address the specific difference
8. **Repeat**: Move to the next category of draw calls
