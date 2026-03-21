# SHORT4 Vertex Position Decoding in D3D9 and Its Impact on RTX Remix Geometry Hashing

## What D3DDECLTYPE_SHORT4 Actually Means in Direct3D 9

In Direct3D 9, `D3DDECLTYPE_SHORT4` is not a “compressed float” type by itself; it is a *vertex fetch format* describing how the GPU (or the D3D runtime’s vertex input stage) expands raw vertex-buffer bytes into the 4‑component vector (`v#`) that the vertex shader (or fixed function emulation shader) receives. The official D3D9 type definition is explicit: **`D3DDECLTYPE_SHORT4` is a four-component signed 16‑bit integer expanded to `(value, value, value, value)`** (i.e., the integer values are converted to floating point without normalization). citeturn30search1turn30search4

This detail matters for your proxy because it means:

* If the original game’s vertex declaration says `SHORT4` for POSITION, the vertex shader input register will already contain **float** values whose magnitudes are on the order of the original `int16` range, not `[-1,1]`. citeturn30search1turn30search4  
* Therefore, *any conversion from those float-ish “raw integer magnitudes” into meaningful object-space coordinates is engine-defined and must occur via shader math* (typically scale and/or bias). The *normalized* variant exists (`D3DDECLTYPE_SHORT4N`) and is explicitly defined as division by `32767.0` per component, but your case is **not** that type. citeturn30search1turn30search4

So the “SHORT4 decoding problem” in D3D9 games is usually not about reproducing D3D’s fetch rules (those are well-defined), but about reconstructing the **engine’s post-fetch unpack (mul/mad) and any subsequent skinning/deformation** that the programmable vertex shader performs before the model/world/view/projection transforms.

## D3D9-Era SHORT4 Position Encoding Conventions and What We Can Infer for cdcEngine

### Common patterns in 2004–2008 engines  
From the D3D9/SM2.0 generation onward, vertex bandwidth reductions (positions in 16-bit domains, normals in bytes, UVs in 16-bit fixed/half) were widely used, with the “decompression” intended to be cheap in a vertex shader—often just multiply-adds per component. A 2008 writeup on vertex component packing (in a shipping-era context) describes exactly this tradeoff: “Some vertex components need to be scaled to the proper range in the vertex shader,” and this is framed as “at most one multiply-add operation per component.” citeturn36search2

A more formal, research-oriented perspective on quantized vertex attributes also highlights the “scale and bias” model: after quantization, the representation includes integer-domain coordinates plus a **scale and bias** used to map those integers back into the original coordinate system. citeturn38view0

In practice, D3D9-era SHORT4 position schemes in shipped engines commonly fall into three families:

* **Raw signed integers with an implicit or explicit scale** (e.g., multiply by `scaleX/Y/Z` stored per mesh or per batch). This is exactly the “multiply only” form.
* **Quantized bounding-box encoding** (bias+scale): `(short → [0..N] or [-N..N])`, then `pos = short * scale + bias`, where `scale` is extent/(2^bits-1) and `bias` is min corner (or center-based). This is the “multiply-add (mad)” form emphasized by both practice and quantization literature. citeturn36search2turn38view0
* **Normalized SNORM-style packing** (SHORT4N), where the *D3D fetch stage itself normalizes* and the shader then rescales to object units. This is explicitly defined in the D3D9 decl type enumeration (divide by `32767.0`). citeturn30search1turn30search4

Your situation is especially suggestive because the game uses **`SHORT4` (unnormalized)** rather than **`SHORT4N`**.

### Concrete evidence from TR7AE modding tooling (Legend/Anniversary pipeline)  
A key, directly relevant public artifact is the Noesis import/export tooling for the **TR7AE mesh format** used by the Legend/Anniversary-era pipeline. The script reads per-model floats named `scaleX`, `scaleY`, `scaleZ`, then reads the vertex position components as **`int16`** and multiplies each axis by its corresponding scale. citeturn29view0turn27view0

Specifically, in the mesh-reading path shown, the workflow is:

1. Read `scaleX/scaleY/scaleZ` as floats from the mesh header. citeturn29view0turn27view0  
2. For each vertex, read `vx/vy/vz` as signed 16-bit values and compute:
   * `vx = readShort() * scaleX`
   * `vy = readShort() * scaleY`
   * `vz = readShort() * scaleZ` citeturn29view0turn27view0  

This is strong evidence that, at least for the classic/model asset pipeline in this engine family, **positions are stored as signed 16-bit integers plus a per-model axis scale**, with no explicit bias in the shown decode step. citeturn29view0turn27view0

The same tool also shows that a 16-bit `bone_id` exists per vertex and that vertex positions and normals are transformed by bone matrices (i.e., doing a skinning-like operation) inside the loader. It explicitly transforms a vertex position by a bone matrix: `vertpos = bones[bone_id].getMatrix().transformPoint([vx, vy, vz])`. citeturn29view0

That suggests a plausible runtime vertex-shader structure in the shipped game: **(a)** expand `SHORT4` (or a related short-based attribute) to float-domain values, **(b)** apply per-mesh/per-batch scaling, and then **(c)** optionally apply bone transforms (skinning) for animated meshes. citeturn30search1turn29view0

Two important caveats emerge from the same modding ecosystem:

* The tooling explicitly warns that “Next Gen” models use a different mesh format that is “not entirely understood.” If your PC build is using the “Next Gen” rendering path, you should expect vertex packing/decoding differences versus the basic TR7AE format. citeturn23view1  
* Public engine-format documentation efforts exist (e.g., `.drm` container structure and relocations), but those describe the container mechanisms more than the GPU-time vertex shader decode. citeturn23view0

In short: the best public, concrete clue for the Legend/Anniversary-era pipeline is **signed-short positions multiplied by per-model scaling factors**, plus an independent per-vertex bone identifier used for bone-space transforms. citeturn29view0turn27view0

## The W Component in SHORT4 Positions: What It Usually Means and What It Could Mean Here

Because `SHORT4` expands to four floats `(x, y, z, w)`, the **W component is “real data”** (a signed 16-bit integer converted to float), not an implicit 1.0 like some other declaration types that expand missing components to defaults. citeturn30search1turn30search4

Historically, engines used the fourth component of a packed position in a few ways:

1. **Padding / sentinel**: always 0 or 1 (after integer→float expansion).  
2. **Secondary payload**: a small integer field, commonly a bone index, blend shape selector, or other per-vertex “tag,” allowing one fewer vertex element fetch.  
3. **Per-vertex scale or exponent**: rarer for positions, but sometimes used in compact “block-compressed” position schemes where W helps reconstruct magnitude.

Direct3D 9 also provides **dedicated vertex declaration usages** for skeletal animation data—`D3DDECLUSAGE_BLENDWEIGHT` and `D3DDECLUSAGE_BLENDINDICES`. citeturn11search0  
So storing bone data in `POSITION.w` is not “the standard way,” but it is absolutely feasible if the engine has full control over the shader signature.

The TR7AE tooling evidence shows a distinct per-vertex 16-bit `bone_id` and (in some cases) a second bone/weight path. citeturn29view0  
That does not prove that `bone_id` is stored in `SHORT4.w` in the D3D9 runtime layout—but it makes it a **top hypothesis** if you are observing `D3DDECLTYPE_SHORT4` for POSITION in live vertex buffers and you can’t find a separate `BLENDINDICES` element.

A practical heuristic you can apply at runtime (no shader source required) is: sample a few hundred vertices and examine the distribution of `wShort`:

* If `wShort` is almost always `0`, `1`, or `-1`, it is likely padding/sentinel.
* If `wShort` is a small non-negative integer with a tight range (e.g., `< 256`, `< 1024`) and correlates with animation state, it is very likely an index (bone, morph target, etc.).
* If `wShort` varies like the other coordinate components (wide signed range, strong correlation with spatial extent), it may be a true 4D position (rare for typical meshes) or part of a more complex reconstruction scheme.

The key point is that **D3D9 itself does not attach semantic meaning to the W of `SHORT4`; it only defines the expansion rule**. citeturn30search1turn30search4

## Recovering Decode Parameters Without HLSL Source Code

When the vertex shader is only available as compiled bytecode, the problem becomes: (1) identify the arithmetic relationship between `v#` inputs and the decoded position, and (2) capture the scale/bias (and possibly bone palette) values used at each draw.

### Disassembling shader bytecode to locate the decode math  
D3D9 provides a standard way to disassemble shader bytecode: `D3DXDisassembleShader` “returns a buffer containing the disassembled shader,” i.e., human-readable assembly. citeturn37search0turn37search4

Microsoft’s own D3D9 shader guidance also emphasizes an important architectural point: the runtime only deals with the compiled shader model binary/assembly; HLSL is not part of the runtime contract. citeturn37search11  
This is why disassembly is the right level of analysis: you want to find the **actual** `mul`/`mad` instructions and which constant registers (`c#`) participate.

For SHORT4 position decoding, the patterns you most often see in disassembly are variants of:

* `mul r0.xyz, v0, cN.xyz`  (scale only)
* `mad r0.xyz, v0, cN.xyz, cM.xyz` (scale+bias)
* sometimes followed by matrix multiply (object/world/view/proj)
* and, for skinning, indexed palette access patterns built from blend indices/weights (harder in SM2.0, but still commonly implemented via constant arrays and dot products)

### Capturing scale/bias constants at runtime via SetVertexShaderConstantF  
Even if you identify that `c12` (for example) is the scale vector, you still need its per-draw values. D3D9 exposes constant updates via `IDirect3DDevice9::SetVertexShaderConstantF`, which sets floating-point constants in a register range `[StartRegister, StartRegister + Vector4fCount)`. citeturn37search5

So one proven approach for a proxy DLL is:

1. Intercept `CreateVertexShader` to capture the bytecode blob.
2. Disassemble it (or parse it) to determine which `c#` registers are used in the decode stage. citeturn37search0turn37search4  
3. Intercept `SetVertexShaderConstantF` to record the values assigned to those registers immediately prior to each draw call of interest. citeturn37search5

A subtle but crucial caveat is that shader compilers can strip unused constants and remap registers; you cannot assume original HLSL constant ordering if you don’t have reflection info. Community discussions on D3D9 constant handling note that unused constants can be removed and remaining ones remapped (e.g., something expected to be `c10` might become `c0`). citeturn37search13  
This makes the “disassemble + intercept constant sets” approach more robust than trying to guess based on asset pipelines alone.

A nice “existence proof” of this runtime-constant pattern (not about SHORT4 specifically, but about the *technique*) is the classic DirectX 9 half-pixel offset fix: it explicitly advises setting a vertex shader constant (e.g., `c255`) at runtime whenever the viewport changes. citeturn36search1  
That demonstrates how normal it is for D3D9 engines to push critical decode/transform parameters through constant registers rather than embedding them into vertex streams.

### Empirical reverse-engineering when constants are hard to isolate  
If disassembly/constant interception is blocked (e.g., heavy state churn, multiple shader variants), you can still empirically solve for scale and bias:

* If you can identify a static mesh whose dimensions in world units are known (or can be measured from gameplay), you can solve `pos = short * scale + bias` by fitting scale and bias that maps observed min/max. This aligns with the established quantization model where integer coordinates are converted back by scale and bias. citeturn38view0  
* If you suspect “scale only” (no bias), a single known edge length can solve for axis scales, and the TR7AE tooling strongly suggests the engine family uses per-axis scale factors in at least some pipelines. citeturn29view0turn27view0

## RTX Remix, Fixed-Function Expectations, and Geometry Hashing Implications

### Why fixed-function matters in RTX Remix  
RTX Remix’s public description and documentation emphasize that it primarily targets **DirectX 8/9 games with fixed function pipelines**. It explicitly warns that injecting into other content is “unlikely to work,” and that there is substantial diversity even among DX8/9 FFP titles. citeturn8search0turn8search4

Your proxy plan—nulling shaders and re-emitting through transforms + fixed-function `DrawIndexedPrimitive`—is therefore aligned with what the runtime is designed for, but it creates a new obligation: you must reconstruct positions **exactly as the game would have produced them prior to world/view/projection**, or Remix will “see” different geometry than the original game did.

### How geometry hashes are defined (and what can destabilize them)  
In the dxvk-remix configuration documentation, geometry hashing is not a single opaque magic value; it is controlled by “rule strings” that define which inputs participate in hash generation (positions, indices, texcoords, geometry descriptors, and more). For example, `rtx.geometryGenerationHashRuleString` is described as defining which hashes to generate via the geometry processing engine, and the documented examples include components like `positions`, `indices`, `texcoords`, `geometrydescriptor`, `vertexlayout`, and `vertexshader`. citeturn10search0

Independent runtime logs shown in early issue reports also illustrate that geometry hash generation can include various “positions/indices/texcoords” groupings (including “legacy” variants). citeturn8search2

The practical consequence for your SHORT4 workflow is:

* If the runtime hashes **positions** (it typically does), then any decode mismatch (wrong scale/bias, wrong handedness, wrong bone transform) yields a different hash, breaking mesh identity and replacement stability across frames.
* If hashing includes **vertexlayout** and/or **vertexshader**, then your proxy must ensure your “FFP re-emission” is stable and consistent in layout and shader identity, or adjust rule strings if you’re controlling configs. citeturn10search0

RTX Remix community experience strongly indicates that hash instability is not cosmetic; it can manifest as visible flicker or incorrect behavior. A recent report on flickering emissives explicitly attributes the flicker to the geometry hash changing. citeturn8search14  
There are also feature requests/complaints specific to shader-based mesh capture and hashing, highlighting that programmable shader paths can fail to produce stable or useful hashes in some cases. citeturn8search1

### What we can and cannot confirm about SHORT4 ingestion in dxvk-remix  
From the code side, dxvk-remix implements D3D9 vertex declarations and can construct them either from FVF codes or explicit element arrays; the `GetDeclaration` path copies stored elements and appends `D3DDECL_END`. citeturn33view7  
This confirms that dxvk-remix represents D3D9 vertex element types structurally, not by forcing everything into `FLOAT3` internally at the API boundary. citeturn33view7

However, the specific internal mapping from each `D3DDECLTYPE_*` (including `SHORT4`) to the underlying Vulkan vertex format is not shown in the excerpts captured here, so the strictest statement supported by direct evidence is:

*D3D9 defines the SHORT4 expansion rule, and dxvk-remix preserves vertex declarations as D3D9 vertex-element structures; therefore, any requirement that Remix “must see FLOAT3 positions” is more likely a hashing/processing-layer constraint than a D3D9 declaration constraint.* citeturn30search1turn33view7

In your proxy design, you are choosing CPU conversion to `FLOAT3` specifically to satisfy the “FFP capture” compatibility target, which RTX Remix itself emphasizes. citeturn8search0turn8search4

## Deterministic SHORT4→FLOAT3 Conversion for Hash Stability and Practical Pipeline Choices

### What makes CPU conversion stable (and what breaks it)  
At the pure numeric level, converting `int16` to `float` and multiplying by a `float` scale is deterministic *for a fixed code path* (same operations, same rounding). In practice, hash instability usually comes from differences in **inputs**, not from IEEE 754 randomness:

* **Wrong or fluctuating decode parameters** (scale/bias changes per draw, per LOD, or per material pass).
* **Dynamic vertex buffers** rewritten each frame (particles, skinned meshes, morph targets), which naturally change decoded positions frame-to-frame.
* **Different mesh identity across passes** (e.g., same geometry drawn with slightly different constants or layout), producing multiple hashes for “the same thing.”
* **Different hashing rule composition** (if vertex layout/shader identity is included). citeturn10search0turn8search14

The TR7AE tooling demonstrates a particularly relevant “input variability” class: the same quantized vertex can be transformed into different final positions depending on `bone_id` and bone matrices (and even weights in some cases). citeturn29view0  
If the runtime mesh you are trying to capture is skinned, then without reproducing that bone transform, you are not just off by a scale—you are capturing the wrong surface entirely.

### Recommended deterministic strategy for your proxy (grounded in the evidence above)  
Given the constraints RTX Remix documents (FFP focus) and the evidence for cdc-engine-family scaling, the most robust CPU-side strategy is:

1. **Treat `SHORT4` fetch semantics as defined:** read the four `int16` components, cast to `float` exactly; do *not* divide by `32767` (that would emulate `SHORT4N`, not `SHORT4`). citeturn30search1turn30search4  
2. **Recover decode constants per draw call** by:
   * disassembling the active vertex shader bytecode to locate scale/bias constant registers, using `D3DXDisassembleShader`, and citeturn37search0turn37search4  
   * intercepting `SetVertexShaderConstantF` to capture the values actually used. citeturn37search5  
   Also assume constants may be stripped/remapped; rely on actual disassembly evidence rather than “expected register numbers.” citeturn37search13  
3. **Use a decode model that can represent both “scale only” and “scale+bias”:**
   * `decoded = raw.xyz * scale.xyz` (matches the TR7AE tooling behavior), citeturn29view0turn27view0  
   * or `decoded = raw.xyz * scale.xyz + bias.xyz` (matches the general quantization literature and many AABB-based encodings). citeturn38view0  
4. **Identify what `w` is doing**:
   * If `w` is a bone index (common hypothesis given the presence of 16-bit bone IDs in tools), and if the mesh is truly skinned in-game, then reproducing *only* scale/bias is insufficient. citeturn29view0turn30search1  
   * If you cannot reproduce skinning in FFP, classify such draws as “dynamic/uncapturable” for stable hashing (or capture only for visuals, accepting hash churn and replacement limitations). This is consistent with the fact that Remix has open discussions/requests around better handling of shader- or skeleton-driven content. citeturn8search1turn8search11  
5. **Cache conversions by (VB identity, vertex range, decode-parameter signature)** rather than by VB pointer alone. The moment decode parameters are per-batch constants (likely), they must be part of the cache key, or you will silently reuse wrong decoded floats.

### Alternatives to CPU conversion  
Two alternative directions exist, each with tradeoffs:

**Use a shader-based capture path rather than nulling shaders.** dxvk-remix’s documentation includes features that explicitly reference a “VertexShader Capture mechanism” (e.g., smooth-normal generation for cases where geometry is missing smooth normals, “especially when using the VertexShader Capture mechanism”). citeturn10search0turn7search0  
This suggests that, for some classes of programmable content, letting the runtime follow a capture path that understands the programmable stage may reduce the need for CPU-side reconstruction—at the cost of depending on what shader patterns Remix currently supports.

**GPU-side “generate decoded positions then read back” style approaches.** Direct3D 9 lacks modern stream-out, but vendor-era techniques like “Render to Vertex Buffer” (R2VB) existed, using pixel shaders and special binding conventions to write transform results into buffers. citeturn11search10  
These approaches are considerably more complex than CPU conversion, and they can introduce their own determinism and synchronization issues, but they exist as an escape hatch if you cannot reconstruct shader math reliably on CPU.

---

**Bottom line, supported by the strongest available public evidence:** the most plausible “cdcEngine-era” SHORT4 position reconstruction for Legend/Anniversary content is **signed 16-bit coordinates multiplied by per-mesh (or per-batch) scale factors**, with skeletal animation further transforming those positions based on 16-bit bone identifiers and bone matrices. citeturn29view0turn27view0turn30search1  
To make that compatible with RTX Remix’s FFP expectations and stable geometry hashing, the most proven path is disassembly-guided constant capture (`D3DXDisassembleShader` + intercept `SetVertexShaderConstantF`) so your proxy can reproduce the exact scale/bias (and, where feasible, skinning) that the original vertex shader would have applied. citeturn37search0turn37search5turn37search13turn10search0turn8search14