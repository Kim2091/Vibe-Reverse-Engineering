# Hash Debugger Agent

## Purpose
Systematic workflow for diagnosing and fixing geometry hash instability in TRL + RTX Remix.

## When to Use
- User reports "hashes are flickering" or "colors change in geometry hash view"
- User reports "materials won't stick" or "replacements keep resetting"
- User reports "light placement doesn't persist"

## Diagnostic Workflow

### Phase 1: Identify the Scope

Ask the user:
1. Which geometry is unstable? (world/level geometry, Lara, props, particles, ALL?)
2. Does it flicker constantly or only when moving? (camera move vs Lara move vs always)
3. What does the Geometry Hash debug view look like? (all flickering vs specific objects)

### Phase 2: Classify the Instability

Based on answers:

**All geometry flickers constantly**
→ Likely: Camera transform baked into World matrix
→ Check: Is `SetTransform(D3DTS_WORLD)` changing every frame even for static geometry?
→ Fix: Ensure World matrix is object-only, not WorldView or WVP
→ Verify: Static geometry should have identical World matrix when camera doesn't move

**Only flickers when camera moves**  
→ Confirmed: Camera is leaking into vertex data or World matrix
→ Check: Compare World matrix across two frames with different camera positions
→ Fix: Decompose WVP properly, isolate camera into View matrix

**Only Lara / characters flicker**
→ Likely: Software skinning (CPU-applied bone transforms change vertex data each frame)
→ This is expected behavior for skinned meshes
→ Fix: Accept this. Use `rtx.calculateAxisAlignedBoundingBox = True` for instance tracking

**Only certain world geometry flickers**
→ Likely: Dynamic vertex buffer reuse, or SHORT4 precision issues
→ Check: Is the vertex buffer being recreated each frame? Is there a conversion step?
→ Fix: Cache converted vertex buffers, ensure identical conversion each frame

**Everything flickers including ground/walls**
→ Likely: Vertex buffer data changes (e.g., dynamic VBs rewritten each frame by the game)
→ Check: Hook vertex buffer Lock/Unlock — is the game writing to VBs every frame?
→ Fix: Detect static geometry and cache it in dedicated, stable vertex buffers

### Phase 3: Recommended Diagnostics

Guide the user to add logging in the proxy DLL:

```cpp
// Log World matrix changes for a specific draw call
void LogWorldMatrix(int drawCallId, const D3DXMATRIX& world) {
    static D3DXMATRIX lastWorld[1000] = {};
    if (memcmp(&world, &lastWorld[drawCallId], sizeof(D3DXMATRIX)) != 0) {
        LOG("DrawCall %d: World matrix CHANGED", drawCallId);
        LOG("  Row0: %.6f %.6f %.6f %.6f", world._11, world._12, world._13, world._14);
        // ... log all rows
        lastWorld[drawCallId] = world;
    }
}
```

```cpp
// Log vertex data hash for a specific VB
void LogVertexHash(IDirect3DVertexBuffer9* vb, UINT offset, UINT size) {
    void* data;
    vb->Lock(offset, size, &data, D3DLOCK_READONLY);
    uint64_t hash = XXH64(data, size, 0);
    LOG("VB %p offset=%u size=%u hash=%016llx", vb, offset, size, hash);
    vb->Unlock();
}
```

### Phase 4: Apply Fixes

Based on diagnosis, guide through the specific fix from `references/hash-stability.md`.

After applying a fix, verify:
1. Run game
2. Alt+X → Debug → Geometry Hash
3. Move camera slowly — colors should not change on target geometry
4. Walk Lara to a new position — static geometry colors should stay the same
5. Return to original position — colors should be the same as before

### Phase 5: Persistence Test

Once hashes are stable:
1. Capture a scene (Alt+X → Capture Frame in USD)
2. Place a test material replacement or light via Toolkit
3. Play the game and return to that area
4. Verify the replacement/light is still there and correctly positioned

## Common Fixes Quick Reference

| Symptom | Fix |
|---------|-----|
| All geometry flickers | Separate World from View matrix |
| Camera-move flicker | Remove camera from World matrix |
| Character flicker | Expected for skinned — enable AABB tracking |
| Subtle per-frame drift | Round positions: `rtx.geometryHashGenerationRoundPosTo = 0.01` |
| Random flicker on some meshes | Cache vertex buffers, don't recreate every frame |
| Particle flicker | Tag as particles: `rtx.particleTextures` |
