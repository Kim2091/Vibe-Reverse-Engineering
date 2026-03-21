# Solving SHORT4 vertex decompression for Tomb Raider Legend's RTX Remix proxy

**The D3D9 fixed-function pipeline categorically rejects `D3DDECLTYPE_SHORT4` for position elements — CPU-side vertex buffer conversion in a proxy DLL is the most reliable path to RTX Remix compatibility.** No existing open-source project provides a drop-in solution, but substantial prior art from Cxbx-Reloaded, xoxor4d's Remix compatibility mods, and the DXVK/Wine codebases supply proven architectural patterns. The core challenge reduces to three operations: intercepting vertex shader constants to extract per-mesh scale/offset values, converting SHORT4 positions to FLOAT3 in shadow vertex buffers, and swapping declarations before draw calls reach Remix. This report covers all viable approaches with implementation-ready pseudocode.

## The SHORT4 problem and why the FFP cannot help

Microsoft's documentation is explicit: **`D3DDECLUSAGE_POSITION` with index 0 must use `D3DDECLTYPE_FLOAT3`** in the fixed-function pipeline. `D3DDECLTYPE_SHORT4` (enumeration value **7**, not 6 — SHORT2 is 6) stores 4 signed 16-bit integers occupying 8 bytes per vertex. When the hardware input assembler feeds SHORT4 to a vertex shader, it performs a raw cast: each `int16` value in [-32768, 32767] arrives as the corresponding float (e.g., -15234 becomes -15234.0f). There is **no normalization** — that distinction belongs to `D3DDECLTYPE_SHORT4N` (value 10), which divides by 32767.0f.

Tomb Raider Legend's cdcEngine stores rigid world geometry positions as SHORT4 and uploads per-mesh bounding-box-derived scale and offset constants via `SetVertexShaderConstantF`. The vertex shader decompresses positions with a single multiply-add: `worldPos.xyz = input.position.xyz * c[N].xyz + c[M].xyz`. RTX Remix Issue #287 confirms the game hooks into Remix but produces no visual output, precisely because Remix's FFP-based scene reconstruction cannot interpret the compressed vertex data. xoxor4d independently abandoned a Dark Souls Remix project for the identical reason: "the vertex data sent to the GPU is compressed."

## Approach 1: CPU-side conversion at draw time

This is the **recommended primary approach** — intercept `DrawIndexedPrimitive`, decode SHORT4 positions using captured shader constants, write FLOAT3 results to a shadow vertex buffer, swap the declaration, and forward to Remix.

### Intercepting and tracking state

The proxy DLL wraps `IDirect3DDevice9` as a COM proxy object. Three interception points feed the conversion pipeline:

**SetVertexShaderConstantF** — Shadow all 256 float4 constant registers in a local array. This costs nearly nothing (a `memcpy` of `Vector4fCount * 16` bytes per call) and gives instant access to the scale/offset values at draw time:

```cpp
HRESULT ProxyDevice::SetVertexShaderConstantF(UINT reg, const float* data, UINT count) {
    memcpy(&m_vsConstants[reg], data, count * 16);
    return m_real->SetVertexShaderConstantF(reg, data, count);
}
```

**SetVertexDeclaration** — When the game sets a declaration, retrieve its elements via `GetDeclaration`, scan for any element with `Usage == D3DDECLUSAGE_POSITION` and `Type == D3DDECLTYPE_SHORT4`. Cache a per-declaration struct recording the SHORT4 position offset, original stride, and a pre-built modified declaration with FLOAT3 substituted. Build the modified declaration by replacing the SHORT4 element's `Type` with `D3DDECLTYPE_FLOAT3` and adjusting all subsequent element offsets by +4 bytes (FLOAT3 is 12 bytes vs SHORT4's 8):

```cpp
void BuildModifiedDecl(const D3DVERTEXELEMENT9* src, D3DVERTEXELEMENT9* dst,
                       WORD short4Offset, /*out*/ UINT& newStride) {
    int delta = 0; // offset shift after position element
    for (int i = 0; src[i].Stream != 0xFF; i++) {
        dst[i] = src[i];
        dst[i].Offset += (src[i].Offset > short4Offset) ? delta : 0;
        if (src[i].Usage == D3DDECLUSAGE_POSITION && src[i].Type == D3DDECLTYPE_SHORT4) {
            dst[i].Type = D3DDECLTYPE_FLOAT3;
            delta = 4; // 12 - 8 = 4 extra bytes
        }
    }
    // add terminator, compute newStride from max(Offset + sizeof(Type))
}
```

**DrawIndexedPrimitive** — The main conversion happens here. The proxy reads `MinVertexIndex` and `NumVertices` to know exactly which vertices to convert, avoiding full-buffer scans:

```cpp
HRESULT ProxyDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex,
    UINT minVertex, UINT numVerts, UINT startIdx, UINT primCount)
{
    if (!m_currentDeclHasShort4) // fast path for standard geometry
        return m_real->DrawIndexedPrimitive(type, baseVertex, ...);

    // 1. Lock original VB read-only
    void* srcData;
    m_currentVB->Lock(0, 0, &srcData, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
    
    // 2. Ensure shadow VB exists and is large enough
    UINT needed = numVerts * m_newStride;
    EnsureShadowVB(needed);
    
    // 3. Lock shadow VB for writing
    void* dstData;
    m_shadowVB->Lock(0, needed, &dstData, D3DLOCK_DISCARD);
    
    // 4. Convert vertices
    ConvertVertices(srcData, dstData, baseVertex + minVertex, numVerts,
                    m_vsConstants[SCALE_REG], m_vsConstants[OFFSET_REG]);
    
    m_shadowVB->Unlock();
    m_currentVB->Unlock();
    
    // 5. Swap state and draw
    m_real->SetStreamSource(0, m_shadowVB, 0, m_newStride);
    m_real->SetVertexDeclaration(m_modifiedDecl);
    m_real->SetVertexShader(NULL); // switch to FFP
    HRESULT hr = m_real->DrawIndexedPrimitive(type, 0, 0, numVerts, startIdx, primCount);
    
    // 6. Restore original state
    m_real->SetStreamSource(0, m_currentVB, m_currentOffset, m_origStride);
    m_real->SetVertexDeclaration(m_currentDecl);
    return hr;
}
```

### The per-vertex conversion kernel

Each vertex requires reading 8 bytes of SHORT4 data, computing 3 multiply-adds, and writing 12 bytes of FLOAT3 plus copying any remaining vertex attributes. With SSE4.1 intrinsics, the inner loop processes the SHORT4→FLOAT3 conversion in roughly 4 instructions per vertex:

```cpp
#include <smmintrin.h> // SSE4.1

void ConvertVerticesSIMD(const uint8_t* src, uint8_t* dst,
                         UINT startVert, UINT count,
                         UINT srcStride, UINT dstStride,
                         UINT short4Offset, UINT float3Offset,
                         const float scale[4], const float offset[4])
{
    __m128 vScale  = _mm_loadu_ps(scale);   // {sx, sy, sz, sw}
    __m128 vOffset = _mm_loadu_ps(offset);  // {ox, oy, oz, ow}

    for (UINT i = 0; i < count; i++) {
        const uint8_t* sv = src + (startVert + i) * srcStride;
        uint8_t* dv = dst + i * dstStride;

        // Load 4 × int16 from SHORT4 position
        __m128i shorts = _mm_loadl_epi64((__m128i*)(sv + short4Offset));
        __m128i ints   = _mm_cvtepi16_epi32(shorts);    // sign-extend to int32
        __m128  floats = _mm_cvtepi32_ps(ints);          // int32 → float
        __m128  result = _mm_add_ps(_mm_mul_ps(floats, vScale), vOffset);

        // Store xyz as FLOAT3 (12 bytes) — avoid overwriting adjacent data
        _mm_store_ss((float*)(dv + float3Offset + 0), result);
        _mm_store_ss((float*)(dv + float3Offset + 4),
                     _mm_shuffle_ps(result, result, _MM_SHUFFLE(1,1,1,1)));
        _mm_store_ss((float*)(dv + float3Offset + 8),
                     _mm_shuffle_ps(result, result, _MM_SHUFFLE(2,2,2,2)));

        // Copy non-position vertex attributes (normals, texcoords, etc.)
        // ... memcpy before and after the position element with offset adjustment
    }
}
```

For an SSE2-only fallback (broader compatibility), replace `_mm_cvtepi16_epi32` with manual sign extension: `_mm_srai_epi32(_mm_unpacklo_epi16(shorts, shorts), 16)`.

### Shadow buffer management

Create shadow vertex buffers with **`D3DPOOL_DEFAULT` + `D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY`**. The `D3DLOCK_DISCARD` flag on lock avoids GPU pipeline stalls by letting the driver allocate from an internal ring buffer — double-buffering is unnecessary since DISCARD handles this automatically. For the source (game) vertex buffers, lock with `D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK` to minimize contention. If the game's buffers are in `D3DPOOL_DEFAULT` without `D3DUSAGE_DYNAMIC`, a read-only lock may stall until the GPU finishes — this is acceptable for static geometry that only needs conversion once.

## Approach 2: conversion at buffer creation and unlock time

Instead of converting per-draw-call, intercept the vertex buffer lifecycle earlier. When `CreateVertexDeclaration` detects SHORT4 position elements, flag all subsequently-created vertex buffers on the associated stream. In the `Lock`/`Unlock` wrapper, capture the game's writes and perform conversion on `Unlock`:

```cpp
HRESULT ProxyVertexBuffer::Unlock() {
    HRESULT hr = m_real->Unlock();
    if (m_hasShort4Position && m_lockedForWrite) {
        // Read from the just-unlocked real VB (lock read-only)
        void* srcData;
        m_real->Lock(0, 0, &srcData, D3DLOCK_READONLY);
        void* dstData;
        m_shadowVB->Lock(0, 0, &dstData, D3DLOCK_DISCARD);
        ConvertAllVertices(srcData, dstData);
        m_shadowVB->Unlock();
        m_real->Unlock();
        m_generation++;  // mark shadow as current
    }
    return hr;
}
```

**The challenge**: at Unlock time, the correct scale/offset constants may not yet be set — the game typically uploads shader constants just before the draw call, not when filling vertex buffers. Static world geometry might use the same scale/offset across the entire level, but this isn't guaranteed. Two mitigations exist: defer the actual conversion until the first draw call that references this buffer (lazy evaluation), or re-convert if the scale/offset changes between draws. The draw-time approach (Approach 1) avoids this timing problem entirely.

For **static geometry** (the vast majority of TRL's world meshes), Unlock-time conversion is highly efficient: convert once, cache forever, and only refresh if the buffer is re-locked for writing. Dynamic buffers (characters, effects) must be reconverted each frame regardless of approach.

## Extracting the decompression scale and offset from shader constants

The vertex shader constant registers containing scale and offset must be identified by shader disassembly. Hook `CreateVertexShader` in the proxy to capture every shader the game creates:

```cpp
HRESULT ProxyDevice::CreateVertexShader(const DWORD* pFunction,
                                        IDirect3DVertexShader9** ppShader) {
    LPD3DXBUFFER pDisasm;
    D3DXDisassembleShader(pFunction, FALSE, NULL, &pDisasm);
    LogShaderDisassembly((const char*)pDisasm->GetBufferPointer());
    pDisasm->Release();
    return m_real->CreateVertexShader(pFunction, ppShader);
}
```

In the disassembled VS 2.0/3.0 bytecode, look for the first operation on `v0` (the POSITION input). The decompression will appear as one of these patterns:

```asm
; Pattern A: explicit MAD (multiply-add)
mad r0.xyz, v0.xyz, c4.xyz, c5.xyz     ; pos = short4 * c4 + c5

; Pattern B: separate MUL + ADD
mul r0.xyz, v0.xyz, c4.xyz
add r0.xyz, r0.xyz, c5.xyz

; Pattern C: baked into world-view-projection matrix via dp4
dp4 r0.x, v0, c0    ; composite matrix = decompress * world * view * proj
dp4 r0.y, v0, c1
dp4 r0.z, v0, c2
dp4 r0.w, v0, c3
```

**Pattern C is problematic** — if decompression is baked into the WVP matrix, you cannot extract a pure scale/offset. However, TRL likely uses Pattern A or B because the engine needs world-space positions for per-pixel lighting before the view-projection transform. The scale and offset typically encode the **axis-aligned bounding box** of the mesh chunk: `scale = (bbMax - bbMin) / 65535.0` and `offset = bbMin`, mapping the full signed short range to the mesh's spatial extent.

Once the register indices are identified (say c4 for scale, c5 for offset), the proxy reads them at draw time from its shadow constant array: `float scale[4] = m_vsConstants[4]`. These values change per-draw-call as different mesh chunks have different bounding boxes. A practical approach for TRL: use **RenderDoc or PIX** to capture a frame, inspect the vertex shader disassembly and constant values for several draw calls, and hard-code the register indices in the proxy. The cdcEngine likely uses a fixed shader constant layout across all world geometry shaders.

The **TheIndra55/cdcEngine** decompilation project on GitHub and the community documentation at cdcengine.re provide additional reverse-engineering context, including DRM file format documentation and section types (ShaderLib sections contain compiled vertex shaders).

## Approach 3: RTX Remix SDK direct geometry submission

The Remix SDK (`remix_c.h`) provides `remixapi_CreateTriangleMesh` which accepts an array of `remixapi_HardcodedVertex` structures containing `float3` positions, `float3` normals, `float2` texcoords, and a packed color. This **completely bypasses the D3D9 pipeline**, letting the proxy submit pre-decoded geometry directly to the path tracer:

```cpp
remixapi_MeshInfoSurfaceTriangles meshInfo = {};
meshInfo.vertices_values = decodedVertices;  // array of remixapi_HardcodedVertex
meshInfo.vertices_count  = numVerts;
meshInfo.indices_values  = indexData;
meshInfo.indices_count   = numIndices;
remixapi_MeshHandle mesh;
remixInterface.CreateTriangleMesh(&meshInfo, &mesh);
remixInterface.DrawInstance(mesh, &transform, /*skinning=*/false);
```

**Critical constraint**: the Remix API is **64-bit only**. Tomb Raider Legend is a 32-bit application, so the RemixAPI is only accessible from the Bridge's 64-bit server side, not from the 32-bit proxy DLL directly. GitHub Issue #736 documents crashes when using RemixAPI through the Bridge with newer runtimes. This makes the SDK approach fragile for TRL — viable only if running a 64-bit build or if Bridge integration stabilizes. The **FFP conversion approach is more reliable** for 32-bit games.

## Approach 4: vertex capture with the original shader

Enabling `rtx.useVertexCapture = True` tells Remix to inject transform-feedback code into the game's original vertex shader, capturing post-VS clip-space positions via `VK_EXT_transform_feedback`. Since TRL's vertex shader properly decompresses SHORT4 to float positions, the captured output should be geometrically correct without any proxy intervention.

However, vertex capture has **three documented failure modes** that make it unreliable as a primary strategy:

- **Race conditions and mesh explosions** (Issue #245): Remix occasionally confuses vertex lists from different draw calls, reconstructing meshes from mixed sources. This manifests as geometry "exploding" — vertices from unrelated objects are stitched together.
- **Hash instability**: Post-transform positions change whenever the camera moves (they're in clip space), making geometry hashing for instance tracking unstable. `rtx.useVertexCaptureCalculateAABB` mitigates this but doesn't eliminate it.
- **Incomplete capture**: Some games' vertex shaders simply don't capture properly — Remix "passes through the original graphics" with no ray-traced output (Issue #275).

A **hybrid strategy** is architecturally sound: use CPU-side FFP conversion for world geometry (static, high-vertex-count meshes where stability matters) and vertex capture as a fallback for complex skinned meshes whose vertex shaders perform bone transformations that are hard to replicate in the proxy. Both paths feed into the same frame's scene graph for path tracing — there is no documented restriction against mixing FFP and vertex-capture geometry within a single frame.

## Performance: conversion overhead is negligible for 2006-era geometry

TRL's world geometry budget is typical of 2006-era engines: **50,000–200,000 vertices per frame** across 500–2,000 draw calls, with individual batches ranging from a few dozen to ~10,000 vertices. The SHORT4→FLOAT3 conversion kernel, even without SIMD, involves 3 multiply-adds per vertex — roughly **4 cycles per vertex** with SSE4.1. Converting 200,000 vertices costs ~0.1–0.2 ms on a modern CPU, well within frame budget.

The dominant cost is **memory bandwidth from vertex buffer locking**, not arithmetic. Three strategies minimize this:

**Caching** is the single most impactful optimization. Most world geometry is static — the vertex buffer contents and scale/offset constants don't change between frames. Track a per-buffer "generation counter" incremented on each `Unlock`, and a per-draw "last-converted" stamp combining the buffer generation and the scale/offset values. Skip conversion entirely when the stamp matches. For a typical TRL frame, this reduces conversion to only the first occurrence of each mesh chunk.

**Pool selection** matters for read performance. If the game creates vertex buffers in `D3DPOOL_MANAGED`, the system-memory backing copy allows fast CPU reads without GPU synchronization. If buffers are in `D3DPOOL_DEFAULT`, a `D3DLOCK_READONLY` lock may stall until the GPU finishes reading. In the worst case, create `D3DPOOL_SYSTEMMEM` staging copies for CPU-side reading.

**Batched memcpy** for non-position attributes: when the vertex layout has position at offset 0, the proxy can write the FLOAT3 position, then `memcpy` the remaining stride bytes (normals, texcoords, colors) in a single operation per vertex, keeping the copy loop tight.

## Prior art and reference implementations

No complete SHORT4→FLOAT3 D3D9 proxy solution exists, but several projects provide proven patterns:

**Cxbx-Reloaded** (Xbox emulator) is the closest architectural precedent. It converts Xbox-specific vertex formats (`D3DVSDT_NORMSHORT4`, `D3DVSDT_NORMPACKED3`) to D3D9-compatible FLOAT types entirely on the CPU, rewriting vertex buffers and declarations before submission. The conversion logic in its vertex buffer handling code demonstrates the full pipeline: detect format, allocate shadow buffer, convert, swap declaration.

**xoxor4d's remix-comp-projects** demonstrate the broader pattern of rewriting game rendering for Remix compatibility. The Black Mesa mod includes a **vertex normal unpacking hack** (toggled via `-disable_normal_hack`) that decompresses packed normals at runtime — the same concept applied to normals rather than positions. The Need for Speed Carbon mod uses a `d3d9.dll` proxy architecture directly. xoxor4d's GTA IV mod is the most mature example of the overall approach: ASI-injected hooks intercept draw calls and re-submit geometry through the FFP path.

**elishacloud/DirectX-Wrappers** provides clean D3D9 proxy templates with full `IDirect3DDevice9` interface passthrough — a solid foundation for the proxy DLL structure without needing to hand-write all 119 virtual method forwarders.

**DXVK** maps `D3DDECLTYPE_SHORT4` to `VK_FORMAT_R16G16B16A16_SINT` for GPU-side vertex fetch but performs no CPU-side conversion — the Vulkan shader handles the format natively. **Wine/WineD3D** similarly delegates to `glVertexAttrib4sv`, letting OpenGL hardware convert at fetch time. Neither approach helps for the Remix FFP path, but both confirm the format semantics.

## Recommended implementation strategy for the TRL proxy

The optimal architecture combines draw-time conversion for correctness with aggressive caching for performance:

**Phase 1 — Shader analysis**: Use RenderDoc or the proxy's `CreateVertexShader` hook with `D3DXDisassembleShader` to identify which constant registers hold the position scale and offset. TRL's cdcEngine likely uses a consistent layout across all world-geometry vertex shaders. Hard-code these register indices once identified.

**Phase 2 — Declaration and stream interception**: In `CreateVertexDeclaration`, detect SHORT4 position elements and pre-build a modified FLOAT3 declaration. In `SetVertexDeclaration` and `SetStreamSource`, track the current state.

**Phase 3 — Draw-time conversion**: In `DrawIndexedPrimitive`, check if the current declaration has SHORT4 positions. If so, lock the source VB read-only, lock the shadow VB with DISCARD, run the SSE-accelerated conversion kernel using the scale/offset from the shadowed constant array, swap the stream source and declaration, set `SetVertexShader(NULL)` to enable FFP, draw, and restore state.

**Phase 4 — Caching**: After the first successful conversion, hash the scale/offset constants and buffer generation. On subsequent draw calls with the same buffer and constants, skip conversion entirely and reuse the cached shadow buffer. For static world geometry this eliminates virtually all per-frame conversion overhead.

**Phase 5 — Fallback for complex shaders**: If any vertex shaders perform transformations that are impractical to replicate on the CPU (skeletal animation, morphing), enable `rtx.useVertexCapture` for those specific draw calls while keeping CPU conversion for the dominant static geometry path.

## Conclusion

The SHORT4 decompression problem is fundamentally a format translation issue with a well-defined solution: shadow the shader constants, convert the vertex data, swap the declaration. The per-vertex math is trivial (`multiply-add`), the vertex counts are modest by modern standards, and caching eliminates redundant work. The primary engineering challenge isn't the conversion itself but correctly identifying TRL's constant register layout and handling the full lifecycle of vertex buffer creation, locking, and device-lost recovery. The CPU-side FFP conversion approach avoids all of vertex capture's stability issues and the Remix SDK's 64-bit constraint, making it the clear first choice. With the elishacloud proxy framework as a starting point and xoxor4d's Remix mods as architectural reference, a working prototype requires implementing roughly three key hooks (`SetVertexShaderConstantF`, `SetVertexDeclaration`, `DrawIndexedPrimitive`) plus the ~30-line SIMD conversion kernel.