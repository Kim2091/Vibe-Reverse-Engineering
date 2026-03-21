# D3D9 accuracy audit of Tomb Raider RTX Remix research docs

The user's Google Drive contains **one substantive technical document** (in two identical copies) making verifiable D3D9 claims: **"Deep-Dive Engineering Analysis: Forcing Fixed-Function Pipeline Fallback in the cdcEngine for Tomb Raider: Legend RTX Remix Compatibility."** A second document, "RTX Remix Community Analysis," covers the NVIDIA RTX Remix modding competition but contains no verifiable D3D9 API claims. The technical document is largely sound on high-level D3D9 architecture but contains **three critical errors** in low-level details — all stemming from a single root cause: confusion between the vertex shader prefix (0xFFFE) and the pixel shader prefix (0xFFFF). Several struct byte offsets are also incorrect by significant margins.

---

## Documents found in Google Drive

Exhaustive searches across the user's Drive using queries for "Tomb Raider," "TRL," "D3D9," "RTX Remix," "RenderDoc," "cdcEngine," "shader," "pipeline," "draw call," "SetRenderState," "DrawIndexedPrimitive," "Crystal Dynamics," "fixed function," "vertex buffer," and "texture replacement" returned:

- **"Tomb Raider RTX Remix Modding Research"** (ID: `13k4rk_z...`, created Jan 31 2026) — The primary technical document. Full of D3D9 claims.
- **"Tomb Raider RTX Remix Modding Research"** (ID: `1EnDsH4g...`, created Jan 31 2026) — Identical earlier copy of the above.
- **"RTX Remix Community Analysis"** (ID: `1aDvo8Tw...`, created Aug 6 2025) — Analysis of the NVIDIA RTX Remix Mod Contest. No D3D9 technical claims to verify.
- **Folders**: "TRL Workspace Docs - March 2026," "Tomb Raider - via OpenLara," "Tomb Raider 2 via OpenLara" — Folders containing non-document files (images/videos), no Google Docs inside them.

No additional relevant documents were found. The remainder of this report focuses exclusively on verifying the technical document's D3D9 claims.

---

## The shader version prefix error is the most consequential finding

The document's most dangerous error appears in **three separate places** and would produce broken code if implemented as written. Direct3D 9 encodes shader versions using a major/minor version in the low word and a **type prefix** in the high word: **0xFFFE for vertex shaders** (`D3DVS_VERSION` macro) and **0xFFFF for pixel shaders** (`D3DPS_VERSION` macro). The document consistently applies the vertex shader prefix 0xFFFE to pixel shader values.

**Instance 1** — The document states: *"In a standard DirectX 9.0c environment, these values would be 0xFFFE0300, indicating Shader Model 3.0 support"* when referring to both `VertexShaderVersion` and `PixelShaderVersion`. The correct value for `PixelShaderVersion` SM 3.0 is **0xFFFF0300**, not 0xFFFE0300.

**Instance 2** — The Ghidra decompiler code example compares `capsStruct.PixelShaderVersion < 0xfffe0200`. The correct threshold for Pixel Shader 2.0 is **0xFFFF0200**. As written, this comparison is **logically broken**: any valid `PixelShaderVersion` (which always carries 0xFFFF in its high word) would be numerically greater than 0xFFFE0200, so the `< 0xfffe0200` branch would never execute — the fallback path would never trigger.

**Instance 3** — By implication, the claim that zeroing `PixelShaderVersion` to 0x00000000 reports no shader support is functionally correct in practice, but Microsoft's driver-level documentation uses `D3DPS_VERSION(0,0)` = 0xFFFF0000, not raw zero. The practical effect is the same because engines typically check the version sub-fields, but the document's reasoning about the encoding is flawed.

| Claim | Document's Value | Correct Value | Verdict |
|:------|:-----------------|:--------------|:--------|
| VS 3.0 encoding | 0xFFFE0300 | 0xFFFE0300 | **Accurate** |
| PS 3.0 encoding | 0xFFFE0300 | **0xFFFF0300** | **Inaccurate** |
| PS 2.0 comparison threshold | 0xFFFE0200 | **0xFFFF0200** | **Inaccurate** |
| Zero = no shader support | 0x00000000 for both | Works in practice; pedantically 0xFFFE0000/0xFFFF0000 | **Partially accurate** |

---

## D3DCAPS9 byte offsets are wrong by +52 bytes

The document claims `VertexShaderVersion` sits at byte offset **248** and `PixelShaderVersion` at **256** within the D3DCAPS9 structure. Counting through all members of D3DCAPS9 (each is a 4-byte DWORD, UINT, float, or enum), the actual offsets are **196** and **204** respectively. The relative gap of 8 bytes between them is correct (there's one intervening member, `MaxVertexShaderConst`), but the absolute positions are inflated by 52 bytes. This error would cause a raw-memory-patching approach to corrupt the wrong fields.

A second offset error: `MaxSimultaneousTextures` is claimed to be at offset **24**. The member at offset 24 is actually `CursorCaps`. `MaxSimultaneousTextures` resides at offset **152**. The `DevCaps` offset of **28** is correct.

| D3DCAPS9 Member | Claimed Offset | Actual Offset | Verdict |
|:----------------|:---------------|:--------------|:--------|
| DevCaps | 28 | 28 | **Accurate** |
| MaxSimultaneousTextures | 24 | **152** | **Inaccurate** |
| VertexShaderVersion | 248 | **196** | **Inaccurate** |
| PixelShaderVersion | 256 | **204** | **Inaccurate** |

---

## VTable indices are all correct — a bright spot

Every VTable index claim in the document checks out against the method declaration order in `d3d9.h` and is corroborated by multiple independent open-source hooking projects (Mumble, OBS Studio, GuidedHacking reference tables).

| Interface Method | Claimed Index | Actual Index | Verdict |
|:-----------------|:--------------|:-------------|:--------|
| IDirect3D9::GetDeviceCaps | 14 | 14 | **Accurate** |
| IDirect3D9::CreateDevice | 16 | 16 | **Accurate** |
| IDirect3DDevice9::DrawPrimitive | 81 | 81 | **Accurate** |
| IDirect3DDevice9::SetVertexShader | 92 | 92 | **Accurate** |
| IDirect3DDevice9::SetPixelShader | 107 | 107 | **Accurate** |

The Ghidra code example's byte offset `*pD3D + 0x38` for `GetDeviceCaps` is also correct: index 14 × 4 bytes per pointer (32-bit) = 56 = 0x38. The call signature `(pD3D, 0, 1, &capsStruct)` correctly maps to `GetDeviceCaps(this, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pCaps)`.

---

## Fixed-function pipeline claims are mostly accurate with minor caveats

The document's high-level architectural descriptions of the D3D9 FFP are well-grounded. Here is the detailed breakdown.

**Fully accurate claims:**

- **"D3D9 is a COM-based API, interfaces accessed through VTables"** — Confirmed. IDirect3D9 and IDirect3DDevice9 both inherit IUnknown, and all D3D9 interfaces use VTable dispatch per the COM specification.
- **"SetTextureStageState defines how multiple texture layers are combined"** — Confirmed. Microsoft documents this as the mechanism for FFP multi-texture blending across up to 8 stages (D3DTSS_COLOROP, D3DTSS_ALPHAOP, etc.).
- **"SetTransform for World, View, and Projection matrices"** — Confirmed. `D3DTS_WORLD`, `D3DTS_VIEW`, and `D3DTS_PROJECTION` are documented transform state types.
- **"V_clip = V_model × World × View × Projection"** — Confirmed. D3D9 uses row-vector left-to-right multiplication in this exact sequence.
- **"FFP uses Gouraud shading"** — Confirmed. Microsoft's own documentation uses the term "Gouraud shading" in the context of FFP lighting and FVF vertex examples.
- **"D3DCREATE_HARDWARE_VERTEXPROCESSING flag"** — Confirmed as a documented, required BehaviorFlags option for `CreateDevice`.
- **"D3DDEVCAPS_HWTRANSFORMANDLIGHT in DevCaps"** — Confirmed. Documented as indicating hardware transform and lighting support.
- **"DLL proxy side-loading"** — Confirmed. Windows DLL search order documentation states the application directory is searched before the system directory for non-Known DLLs like `dinput8.dll`.
- **"D3DPRESENT_PARAMETERS"** — Confirmed as a documented D3D9 structure used with `CreateDevice`.

**Partially accurate claims:**

- **"Fixed-Function Lights (8 max)"** — The D3D9 API does **not** define a fixed maximum of 8 lights. The actual limit is hardware-dependent and reported via `D3DCAPS9.MaxActiveLights`. While **8 was extremely common** on era-appropriate hardware, some devices supported fewer or more. The document presents this as a hard API limit rather than a typical hardware capability.

- **"Passing NULL to SetVertexShader and SetPixelShader forces FFP fallback"** — For **vertex shaders**, Microsoft explicitly documents: *"call SetVertexShader(NULL) to release the programmable shader, and then call SetFVF with the fixed-function vertex format."* The document omits the required `SetFVF` call. For **pixel shaders**, the `SetPixelShader(NULL)` behavior is not explicitly documented at the public API level — it is only confirmed in driver-level documentation (`PFND3DDDI_SETPIXELSHADER` mentions that handle value 0 indicates the fixed-function pipeline). The practical effect described is correct, but the vertex shader path is incomplete and the pixel shader path is underdocumented.

- **"Software Vertex Processing fallback"** — SVP is a real, well-documented D3D9 feature (`D3DCREATE_SOFTWARE_VERTEXPROCESSING`). The claim that engines may fall back to SVP when detecting no shader support is plausible but not documented by Microsoft. The claim that SVP is "detrimental for RTX Remix" pertains to NVIDIA's tool, not D3D9 itself, and cannot be verified from Microsoft documentation.

---

## Game-specific and RTX Remix claims fall outside Microsoft's scope

Several claims in the document relate to game-specific implementation details or NVIDIA RTX Remix behavior. These are categorized as **unverifiable** against Microsoft Learn because they describe proprietary engine behavior or third-party tool functionality:

- The cdcEngine's internal "Next-Gen Content" toggle and its tiered rendering path selection
- The registry key `HKEY_CURRENT_USER\Software\Crystal Dynamics\Tomb Raider: Legend` controlling the Next-Gen toggle
- RTX Remix configuration parameters (`d3d9.shaderModel`, `rtx.forceVertexShaderNull`, etc.)
- The claim that RTX Remix intercepts FFP commands and replaces them with path-traced rendering
- The claim that RTX Remix requires hardware TnL and cannot work with SVP
- The assertion that the Kazakhstan level crashes stem from vertex buffer mismatches between rendering paths
- Nixxes-specific performance patches modifying `D3DPRESENT_PARAMETERS` handling

These claims may be accurate within their respective domains (community modding knowledge, NVIDIA tooling), but they are outside the scope of Microsoft's D3D9 API documentation.

---

## Complete claim verification summary

| # | Claim | Category | Verdict |
|:--|:------|:---------|:--------|
| 1 | GetDeviceCaps returns D3DCAPS9 | D3DCAPS9 | ✅ Accurate |
| 2 | VertexShaderVersion at byte offset 248 | D3DCAPS9 | ❌ **Inaccurate** — actual offset is 196 |
| 3 | PixelShaderVersion at byte offset 256 | D3DCAPS9 | ❌ **Inaccurate** — actual offset is 204 |
| 4 | Both versions = 0xFFFE0300 for SM 3.0 | D3DCAPS9 | ⚠️ **Partially accurate** — VS correct, PS should be 0xFFFF0300 |
| 5 | Zero values = no shader support | D3DCAPS9 | ⚠️ **Partially accurate** — works in practice |
| 6 | MaxSimultaneousTextures at offset 24 | D3DCAPS9 | ❌ **Inaccurate** — actual offset is 152 |
| 7 | DevCaps at offset 28 | D3DCAPS9 | ✅ Accurate |
| 8 | D3DDEVCAPS_HWTRANSFORMANDLIGHT flag | D3DCAPS9 | ✅ Accurate |
| 9 | PixelShaderVersion < 0xFFFE0200 in code | D3DCAPS9 | ❌ **Inaccurate** — should be 0xFFFF0200; comparison is logically broken |
| 10 | D3D9 is COM-based with VTables | VTable | ✅ Accurate |
| 11 | IDirect3D9::GetDeviceCaps at index 14 | VTable | ✅ Accurate |
| 12 | IDirect3D9::CreateDevice at index 16 | VTable | ✅ Accurate |
| 13 | IDirect3DDevice9::SetVertexShader at index 92 | VTable | ✅ Accurate |
| 14 | IDirect3DDevice9::SetPixelShader at index 107 | VTable | ✅ Accurate |
| 15 | IDirect3DDevice9::DrawPrimitive at index 81 | VTable | ✅ Accurate |
| 16 | Ghidra offset 0x38 = GetDeviceCaps (14 × 4) | VTable | ✅ Accurate |
| 17 | SetTextureStageState controls FFP blending | Pipeline | ✅ Accurate |
| 18 | SetTransform for World/View/Projection | Pipeline | ✅ Accurate |
| 19 | V_clip = V_model × W × V × P | Pipeline | ✅ Accurate |
| 20 | FFP uses Gouraud shading | Pipeline | ✅ Accurate |
| 21 | Fixed-function lights max 8 | Pipeline | ⚠️ **Partially accurate** — hardware-dependent via MaxActiveLights |
| 22 | NULL to SetVertexShader/SetPixelShader = FFP | Pipeline | ⚠️ **Partially accurate** — VS requires SetFVF too; PS NULL underdocumented |
| 23 | D3DCREATE_HARDWARE_VERTEXPROCESSING | Pipeline | ✅ Accurate |
| 24 | Software Vertex Processing exists | Pipeline | ⚠️ **Partially accurate** — SVP is real; RTX Remix part unverifiable |
| 25 | DLL proxy side-loading via app directory | Windows | ✅ Accurate |
| 26 | D3DPRESENT_PARAMETERS structure | Pipeline | ✅ Accurate |

**Final tally: 16 accurate, 4 partially accurate, 4 inaccurate, 2 partially accurate (with unverifiable components).** The document's architectural understanding is strong, but anyone using the specific byte offsets or shader version constants for actual code should correct the errors identified above before implementation.

---

## Conclusion: solid architecture, dangerous constants

The document demonstrates genuine expertise in D3D9's high-level architecture. Its VTable indices, COM model descriptions, FFP pipeline mechanics, and transformation mathematics all hold up against Microsoft's official documentation. The strategic approach of spoofing `D3DCAPS9` to force an FFP fallback is technically sound.

The danger lies in the **specific numeric constants**. Three of the four inaccurate claims would produce silent, hard-to-debug failures: writing to byte offsets 248/256 instead of 196/204 would corrupt unrelated struct members, and comparing `PixelShaderVersion` against 0xFFFE0200 would create a condition that never triggers. The consistent misuse of the 0xFFFE prefix for pixel shaders (which require 0xFFFF) suggests the author relied on a single incorrect source for shader version encoding rather than consulting Microsoft's `D3DPS_VERSION`/`D3DVS_VERSION` macro documentation directly. Anyone implementing the techniques described in this document should replace these constants with values derived from the official D3D9 SDK headers.