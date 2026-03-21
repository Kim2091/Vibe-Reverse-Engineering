# RTX.conf and DXVK.conf Reference for Tomb Raider Legend

## Starter rtx.conf for TRL

```ini
# ═══════════════════════════════════════════════════════════════
# Tomb Raider Legend — RTX Remix Configuration
# Place next to trl.exe (or your game executable)
# ═══════════════════════════════════════════════════════════════

# ── Core Ray Tracing ──────────────────────────────────────────
rtx.enableRaytracing = True
rtx.enablePathTracing = True

# ── Transform / Matrix Setup ─────────────────────────────────
# Mode 0: Separate W, V, P (use when proxy provides clean matrices)
# Mode 1: World contains World×View
# Mode 2: World contains World×View×Projection  
rtx.fusedWorldViewMode = 0

# Coordinate system (False = Y-up, True = Z-up)
rtx.zUp = False

# ── Vertex Capture ────────────────────────────────────────────
# Enable if NOT using FFP proxy (captures post-VS output)
# Disable if using FFP proxy (vertices already correct)
rtx.useVertexCapture = False

# ── Geometry Hashing ──────────────────────────────────────────
# Round positions to reduce floating point noise
rtx.geometryHashGenerationRoundPosTo = 0.01

# Round texture coordinates
rtx.geometryHashRoundTextureCoordinatesTo = 0.0001

# Calculate AABB for better instance tracking
rtx.calculateAxisAlignedBoundingBox = True

# Detect hash collisions (debugging)
rtx.hashCollisionDetection = True

# ── Anti-Culling ──────────────────────────────────────────────
# Keep geometry alive that the game would normally cull
rtx.antiCulling.object.enable = True
rtx.antiCulling.object.numFramesToExtendObjectLifetime = 120
rtx.antiCulling.object.fovScale = 2.0

# Keep lights alive
rtx.antiCulling.light.enable = True
rtx.antiCulling.light.numFramesToExtendLightLifetime = 120
rtx.antiCulling.light.fovScale = 3.0

# ── UI Textures ───────────────────────────────────────────────
# Add TRL UI texture hashes here (hex, comma-separated)
# Find these using Alt+X → Game Setup → UI Textures
rtx.uiTextures = 

# ── Skybox ────────────────────────────────────────────────────
rtx.skyBoxTextures = 
# rtx.skyAutoDetect = True

# ── Lighting ──────────────────────────────────────────────────
# Fallback light (illuminates scene when no explicit lights exist)
rtx.fallbackLightMode = 2
# 0 = None, 1 = Constant, 2 = Distant (sun-like)
rtx.fallbackLightRadiance = 5.0 5.0 5.0
rtx.fallbackLightDirection = 0.3 -0.8 0.5

# Ignore baked vertex color lighting (TRL may bake lightmaps into vertex colors)
rtx.ignoreAllVertexColorBakedLighting = True

# Remove last texture stage (often a lightmap in this era of games)
rtx.ignoreLastTextureStage = True

# ── Game Light Handling ───────────────────────────────────────
# Set these based on what TRL provides via D3D9 light API
rtx.ignoreGameDirectionalLights = False
rtx.ignoreGamePointLights = False
rtx.ignoreGameSpotLights = False

# Fixed light radius for legacy lights converted to sphere
rtx.legacyLight.sphereRadius = 0.05

# ── Culling ───────────────────────────────────────────────────
rtx.enableCulling = False
# Disable initially until you understand what TRL submits
# Re-enable selectively once rendering is stable

# ── Performance ───────────────────────────────────────────────
rtx.qualityDLSS = 3
# 1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance

# ── Near Plane ────────────────────────────────────────────────
rtx.enableNearPlaneOverride = True
rtx.nearPlaneOverride = 0.1

# ── Orthographic UI Detection ─────────────────────────────────
rtx.orthographicIsUI = True
# Auto-detect orthographic projections as UI (skip ray tracing)

# ── Debug ─────────────────────────────────────────────────────
rtx.enableDebugMode = True
# Enable for development; disable for release
# Access debug views with Alt+X → Debug

# ── Capture ───────────────────────────────────────────────────
# rtx.captureHotKey = VK_F10
# rtx.captureInstances = True
```

## Starter dxvk.conf

```ini
# ═══════════════════════════════════════════════════════════════
# DXVK Configuration for Tomb Raider Legend + RTX Remix
# Place next to trl.exe
# ═══════════════════════════════════════════════════════════════

# Force D3D9 feature level (may help with shader compatibility)
# d3d9.shaderModel = 2

# Force software vertex processing (may help capture transforms)
# d3d9.forceSoftwareVertexProcessing = True

# Strict float emulation (fixed flickering in TRL per DXVK upstream)
d3d9.floatEmulation = Strict

# Enable Aftermath for GPU crash dumps
# dxvk.enableAftermath = True
```

## Important rtx.conf Options Reference

### Texture Classification Hashes
These are hex hashes you discover via the Dev Menu → Game Setup:

```ini
rtx.uiTextures = HASH1, HASH2, ...
rtx.worldSpaceUITextures = HASH1, HASH2, ...
rtx.particleTextures = HASH1, HASH2, ...
rtx.beamTextures = HASH1, HASH2, ...
rtx.lightmapTextures = HASH1, HASH2, ...
rtx.decalTextures = HASH1, HASH2, ...
rtx.dynamicDecalTextures = HASH1, HASH2, ...
rtx.singleOffsetDecalTextures = HASH1, HASH2, ...
rtx.animatedWaterTextures = HASH1, HASH2, ...
rtx.skyBoxTextures = HASH1, HASH2, ...
rtx.ignoreLightTextures = HASH1, HASH2, ...
rtx.hideInstanceTextures = HASH1, HASH2, ...
rtx.playerModelTextures = HASH1, HASH2, ...
rtx.playerModelBodyTextures = HASH1, HASH2, ...
```

### Hotkeys (Customizable)
```ini
rtx.userGraphicsSettingMenuHotkey = VK_MENU, VK_X
rtx.developerSettingsMenuHotkey = VK_MENU, VK_Y
rtx.captureHotKey = VK_F10
```

### Remix Mod Paths
```ini
rtx.baseGameModPath = rtx-remix/mods/gamemod
# Path to your mod layer (USD files with replacements/lights)
```

## Iterative Configuration Workflow

1. **Start minimal**: Just `rtx.enableRaytracing = True` and your FFP proxy
2. **Tag UI textures first**: This is step 1 in any Remix setup
3. **Check geometry hash stability**: Debug view, fix proxy if needed
4. **Tag sky/particles**: Clean up what Remix sees
5. **Tune anti-culling**: Increase lifetime/FOV until geometry stops popping
6. **Add lights via Toolkit**: Once hashes are stable
7. **Tune quality**: DLSS, denoiser settings, exposure, volumetrics
8. **Optimize performance**: Reduce anti-culling conservatism, enable culling selectively
