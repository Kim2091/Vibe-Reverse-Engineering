# Installing GhidrAssist in Ghidra

**GhidrAssist** is the LLM-powered Ghidra plugin (chat, explain, Graph-RAG, MCP client). It runs **inside** Ghidra. You install it as a Ghidra extension.

## Option A: Install from a prebuilt ZIP (recommended)

1. **Get the extension ZIP**
   - Download a release from [GhidrAssist Releases](https://github.com/jtang613/GhidrAssist/releases), e.g. `GhidrAssist-vX.X.X.zip`,  
   - or build it yourself (see Option B) and use the ZIP from `dist/`.

2. **Install in Ghidra**
   - Start **Ghidra** (your normal install, e.g. `ghidra_12.0.1_PUBLIC` or the 20260314 bundle).
   - **File → Install Extensions…**
   - **Add Extension…** and select the downloaded/built `GhidrAssist-*.zip`.
   - Restart Ghidra when prompted.

3. **Enable the plugin**
   - **File → Configure → Configure Plugins**
   - Search for **GhidrAssist**, check the box, OK.

4. **Enable in CodeBrowser**
   - Open a project and open a program in **CodeBrowser**.
   - **File → Configure → Miscellaneous** and enable **GhidrAssist**.
   - **Window → GhidrAssist** to open the UI.

5. **Configure**
   - **Tools → GhidrAssist Settings**: set API host, API key, and (optional) RLHF/RAG paths.

## Option B: Build from your existing folder

If you use the folder  
`C:\Users\skurtyy\Downloads\ghidra_12.0_PUBLIC_20260314_GhidrAssist\GhidrAssist`:

1. Set **Ghidra install path** (e.g. in PowerShell):
   ```powershell
   $env:GHIDRA_INSTALL_DIR = "C:\path\to\ghidra_12.0.1_PUBLIC"
   ```

2. From the **GhidrAssist** project directory (where `build.gradle` or `gradle` is):
   ```powershell
   cd "C:\Users\skurtyy\Downloads\ghidra_12.0_PUBLIC_20260314_GhidrAssist\GhidrAssist"
   .\gradlew buildExtension
   ```

3. Use the ZIP produced in `dist/` and follow **Option A** from step 2 onward.

## Using GhidrAssist with GhidrAssistMCP

- **GhidrAssistMCP** runs the MCP server **inside Ghidra** (Window → GhidrAssistMCP, then start the server).
- In GhidrAssist: **Tools → GhidrAssist Settings → MCP Servers** tab:
  - Add server URL: `http://127.0.0.1:8080` (or the port you set in GhidrAssistMCP).
  - Transport: **SSE**.
- In the **Custom Query** tab, check **Use MCP** (and optionally **Agentic**) to let the in-Ghidra LLM use GhidrAssistMCP tools.

Cursor uses the same GhidrAssistMCP server (when it’s running in Ghidra) via the `ghidrassistmcp` entry in `~/.cursor/mcp.json`.
