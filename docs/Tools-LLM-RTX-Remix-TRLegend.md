# Tools, debuggers, and MCP servers for Tomb Raider Legend + RTX Remix (LLM-friendly)

Summary of tools that can help an LLM (and you) get information to make Tomb Raider Legend compatible with RTX Remix. Focus: DX9 reverse engineering, FFP/shaders, and AI-accessible analysis.

---

## Already in your workflow

- **Vibe Reverse Engineering toolset** (this repo): `retools/` (static), `livetools/` (Frida), `dumpinfo`/`throwmap`, DX9 FFP template and scripts. Use the Tool Catalog and @dx9-ffp-port for TR Legend FFP porting.
- **GhidrAssistMCP** (now in Cursor): When Ghidra is running with a binary loaded and the MCP server started (Window → GhidrAssistMCP), Cursor can call Ghidra tools (decompile, xrefs, list functions, etc.) via the `ghidrassistmcp` MCP server.

---

## MCP servers (for Cursor / LLM)

| Tool | Role | Notes |
|------|------|--------|
| **GhidrAssistMCP** | Ghidra MCP server | **Installed.** Start in Ghidra (Window → GhidrAssistMCP), then Cursor connects to `http://localhost:8080/sse`. 31+ tools: decompile, list functions/imports/exports, xrefs, rename, set types, etc. |
| **GhidrAssist** | Ghidra plugin (LLM UI) | Install in Ghidra (see `GhidrAssist-Ghidra-Install.md`). Uses GhidrAssistMCP from inside Ghidra; Cursor uses the same MCP from the IDE. |
| **CutterMCP** | Cutter ↔ MCP | If you use Cutter: MCP server for decompilation, renaming, symbol analysis. Add to Cursor config; requires Cutter installed. |
| **GhidraMCP** | Alternate Ghidra MCP | Another Ghidra MCP integration (different from GhidrAssistMCP). Requires Ghidra + Python3 + MCP SDK. |
| **IDA Pro MCP** | IDA ↔ MCP | For IDA Pro 8.3+ (not IDA Free): decompile, disassemble, xrefs, rename, types, comments; optional debugging. Plugin runs inside IDA; Cursor can talk to it. |

For TR Legend + RTX Remix, **GhidrAssistMCP + your Vibe retools/livetools** is the main combo; add IDA or Cutter MCP if you already use those.

---

## Debuggers and capture (DX9 / graphics)

- **RenderDoc**  
  - Often used for D3D11/Vulkan. D3D9 support is limited or absent in many builds; check [RenderDoc docs](https://renderdoc.org/docs/) for your version.  
  - If available, use for frame capture and draw-call inspection once you have a working D3D9 path.

- **PIX (Windows)**  
  - Microsoft’s graphics debugger. Historically strong on D3D12; D3D9 support is limited. Check current PIX docs for D3D9.

- **Your livetools (Frida)**  
  - `dipcnt` / `dipcnt callers`: see who triggers `DrawIndexedPrimitive` and how often.  
  - `trace` / `steptrace` / `bp` + `regs`/`stack`: follow matrix setup, VS constant uploads, and draw paths.  
  - Essential for mapping VS constants (view/proj/world) for the FFP proxy.

- **Minidump + dumpinfo/throwmap**  
  - When the game or proxy crashes: `dumpinfo.py diagnose --binary d3d9.dll` and `throwmap.py` to get exact throw site and error strings.

---

## Community and references (from search)

- **Tomb Raider Forums – RTX Remix**  
  - [RTX-Remix (Tomb Raider Forums)](https://www.tombraiderforums.com/showthread.php) and “Forcing Tomb Raider Legend / Anniversary to render …” – practical fixes and workarounds.

- **GitHub (rtx-remix issues)**  
  - Search for “Tomb Raider Legend” or “Legend” in the RTX Remix org issues for hooking/rendering problems and solutions.

- **Reddit (r/nvidia)**  
  - “Nvidia RTX Remix - extract Dx8/9 game geometry” – high-level DX8/9 capture and geometry.

- **Nexus Mods**  
  - “RTX Remix - Q&A with NVIDIA at Tomb Raider” – community Q&A and modding context.

These are useful for *context* and *reported issues*; for actual RE (matrix slots, vertex formats, draw flow), your Vibe toolset + GhidrAssistMCP + livetools are the main leverage.

---

## Suggested workflow for TR Legend + RTX Remix

1. **Static (Cursor + Ghidra)**  
   - Load game exe and relevant DLLs in Ghidra; start **GhidrAssistMCP** in Ghidra.  
   - In Cursor, use the GhidrAssistMCP tools (decompile, list functions, xrefs) to find D3D9 device usage, `SetVertexShaderConstantF` call sites, and matrix/constant flow.

2. **Dynamic (livetools)**  
   - Attach to the game (or the FFP proxy), use `trace`/`steptrace`/`dipcnt callers` to confirm which code paths run and what constants are passed.  
   - Use this to validate and adjust FFP proxy defines (e.g. `VS_REG_VIEW_START`, `VS_REG_WORLD_START`).

3. **Crashes**  
   - Capture a minidump, run `dumpinfo.py diagnose` and `throwmap.py` with the correct binary to get exact failure point and message.

4. **Docs and rules**  
   - Keep using the **Tool Catalog** and **@dx9-ffp-port** in this repo for FFP porting steps and pitfalls.
