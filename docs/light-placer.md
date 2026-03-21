# Light Placer Agent

## Purpose
Workflow for placing lights on meshes in RTX Remix so they stay fixed in world space 
regardless of camera movement or Lara's position.

## Prerequisites
- Stable geometry hashes (verified via Geometry Hash debug view)
- RTX Remix Toolkit installed
- At least one successful scene capture (.usd)
- Understanding of which meshes are static vs dynamic

## The Golden Rule

**Lights are placed at absolute world coordinates. They don't "attach" to meshes.**

A light "stays on a mesh" because:
1. The mesh is always at the same world position (static geometry)
2. The mesh has a stable hash (Remix recognizes it frame to frame)
3. The light is at a fixed world coordinate that happens to be near/on the mesh

If ANY of those three break, the light appears to "move" relative to the mesh.

## Workflow: Placing Lights on Static World Geometry

### Step 1: Verify Hash Stability
```
1. Launch TRL with proxy + Remix
2. Navigate to target area
3. Alt+X → Debug → Debug View → Geometry Hash
4. Identify the mesh you want to light
5. Move camera around — mesh color must stay constant
6. Walk Lara around — mesh color must stay constant
7. If unstable → fix proxy first (see hash-debugger agent)
```

### Step 2: Capture the Scene
```
1. Position camera to see the target area well
2. Alt+X → Enhancements → Capture Frame in USD
3. Wait for capture to complete
4. File saved to: <game_dir>/rtx-remix/captures/capture_XXXX/
```

### Step 3: Open in Remix Toolkit
```
1. Launch RTX Remix Toolkit (Omniverse app)
2. File → Open → navigate to capture .usd
3. Find the mesh in the viewport or scene graph
4. Note its world position (check Properties panel)
```

### Step 4: Place the Light
```
1. In Toolkit: Add → Light → [Sphere | Rect | Disc | Distant]
2. Position the light at the desired world coordinate
3. For a "stage light on a mesh":
   - Use Rect Light or Disc Light for area illumination
   - Or Sphere Light for point-like illumination
   - Adjust intensity, color, cone angle as needed
4. The light's Transform in the Properties panel shows world coordinates
5. Save the mod layer (File → Save Layer)
```

### Step 5: Verify In-Game
```
1. Ensure mod layer is in rtx-remix/mods/ directory
2. Ensure rtx.conf references the mod:
   rtx.baseGameModPath = rtx-remix/mods/your_mod
3. Launch game
4. Navigate to the area
5. Verify:
   □ Light is at correct position
   □ Move camera → light stays fixed ✓
   □ Walk Lara → light stays fixed ✓
   □ Return to area later → light still there ✓
```

## Workflow: Lights on Moving Objects (Lara, NPCs, Props)

Moving objects change world position each frame. Static USD lights won't follow them.

### Option A: D3D9 API Lights (via Proxy)

Your proxy DLL creates D3D9 lights that update position each frame:

```cpp
// In proxy, each frame after computing Lara's world position:
void UpdateDynamicLights() {
    D3DXVECTOR3 laraPos = GetLaraWorldPosition();  // From your matrix extraction
    D3DXVECTOR3 laraFwd = GetLaraForwardVector();
    
    // Stage light above and behind Lara
    D3DLIGHT9 stageLight = {};
    stageLight.Type = D3DLIGHT_SPOT;
    stageLight.Position.x = laraPos.x - laraFwd.x * 2.0f;
    stageLight.Position.y = laraPos.y + 3.0f;
    stageLight.Position.z = laraPos.z - laraFwd.z * 2.0f;
    stageLight.Direction = {laraFwd.x, -0.3f, laraFwd.z};
    stageLight.Range = 30.0f;
    stageLight.Falloff = 1.0f;
    stageLight.Attenuation0 = 0.0f;
    stageLight.Attenuation1 = 0.05f;
    stageLight.Attenuation2 = 0.0f;
    stageLight.Theta = D3DXToRadian(15.0f);   // Inner cone 15°
    stageLight.Phi = D3DXToRadian(30.0f);     // Outer cone 30°
    stageLight.Diffuse = {1.0f, 0.95f, 0.9f, 1.0f};  // Warm white
    
    m_realDevice->SetLight(STAGE_LIGHT_INDEX, &stageLight);
    m_realDevice->LightEnable(STAGE_LIGHT_INDEX, TRUE);
}
```

Remix converts D3D9 lights to its internal representation and ray-traces them.

### Option B: Remix API Lights

If your proxy integrates with the Remix API:

```cpp
void UpdateRemixLight(remixapi_Interface* remix) {
    D3DXVECTOR3 pos = GetMeshWorldPosition();
    
    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {pos.x, pos.y + 1.0f, pos.z};
    sphere.radius = 0.05f;
    sphere.luminosity = {500.0f, 500.0f, 480.0f};
    sphere.shaping_enabled = false;
    
    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = 0xDEADBEEF12345678;  // Stable hash for this light
    info.pNext = &sphere;
    
    remix->DrawLightInstance(info);
}
```

**Important**: Keep the `.hash` the same every frame for stable denoising.
Change position/intensity freely — Remix uses the hash to track the light.

### Option C: Anchor Asset Method

For moving objects with stable mesh hashes:

1. Create a unique tiny "anchor mesh" with a guaranteed unique hash
2. Render this anchor mesh every frame at the object's world position
3. In the Remix Toolkit, attach lights relative to this anchor
4. When the anchor moves, the light follows (because Remix tracks the instance)

This works because Remix tracks instances by hash + world position. If the same hash
appears at a new position, Remix updates the instance location, and any attached
toolkit lights follow.

## Light Types Reference

| Type | Best For | Key Properties |
|------|----------|----------------|
| Sphere | Point-like sources (bulbs, candles) | position, radius, intensity |
| Rect | Area lights (windows, screens, panels) | position, dimensions, intensity |
| Disc | Round area lights (spotlights, stage lights) | position, radius, direction |
| Distant | Sun/moon (parallel rays) | direction, angle, intensity |
| Cylinder | Tube lights (fluorescents) | position, length, radius |

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Light not visible | Too dim or wrong position | Increase intensity, check coordinates |
| Light visible but moves with camera | World matrix includes View | Fix proxy matrices |
| Light visible but moves when walking | Mesh hash unstable | Fix hash stability |
| Light appears in capture but not in-game | Mod path not set | Check `rtx.baseGameModPath` |
| Light flickers | Hash changes intermittently | Increase `rtx.geometryHashGenerationRoundPosTo` |
| Light bleeds through walls | No occlusion geometry behind wall | Ensure anti-culling keeps back-face geometry |
| Light too harsh / bright | Physically-based units | Reduce luminosity, increase radius |
