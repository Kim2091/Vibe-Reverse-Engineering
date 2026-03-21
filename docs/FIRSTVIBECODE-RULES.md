# Reverse Engineering Workspace Rules

## Game Directory (Tomb Raider Legend root)

Only the following belong at the game root:

### Original Game Files (never touch)
- `trl.exe`, `trl.exe.bak`, `tr7.pdb`, `testapp.exe`
- `bigfile.*`, `*.bik`, `binkw32.dll`
- `installscript.vdf`, `readme.rtf`

### RTX Remix Runtime (needed for Remix to function)
- `.trex/` -- Remix runtime DLLs
- `rtx-remix/` -- captures, logs, mods
- `d3d9_remix.dll` -- Remix bridge
- `NvRemixLauncher32.exe`, `NvRemixLauncher32.pdb`
- `dxvk.conf`, `dxwrapper.dll`, `dxwrapper.ini`, `d3d8_off.dll`
- `rtx.conf`, `user.conf`
- `trl.dxvk-cache`
- `d3d9.pdb` -- current proxy debug symbols
- License/readme files (`artifacts_readme.txt`, `build-names.txt`, `License.txt`, `ThirdPartyLicenses-*.txt`)

### Active Test Files (only during testing)
- `d3d9.dll` -- current FFP proxy under test
- `proxy.ini` -- current proxy config
- `ffp_proxy.log` -- current session log (move to Reverse after test)

Everything else goes into `Reverse/`.

---

## Reverse/ Folder Structure

### `Reverse/logs/`
All log files, organized by source:

| Subfolder | Contents |
|-----------|----------|
| `ffp-proxy/` | FFP proxy logs (`ffp_proxy*.log`) |
| `dx-trace/` | DX9 tracer output (`dxtrace_*.jsonl`, `dxtrace_*.log`, `dxtrace_*.txt`) |
| `remix-runtime/` | Remix runtime logs (`metrics.txt`, `nrc_session_log.txt`) |

### `Reverse/tests/`
Each test is a folder containing the d3d9.dll, proxy.ini, and optionally logs/configs from that test run.

**Naming convention:** `YYYYMMDD-HHMMSS-<Yes|No>-<Description>`

- **Yes** = RTX Remix rendered path-traced geometry
- **No** = Remix hooked but did not render path-traced geometry

Examples:
- `20260315-034534-No-FixedFunction`
- `20260318-143000-Yes-WorldMatrixFixed`

### `Reverse/builds/`
Old proxy DLL versions and build artifacts. Named with timestamps for traceability.

### `Reverse/configs/`
Old proxy.ini, user.conf, and other config backups.

### `Reverse/notes/`
Findings, analysis notes, and observations from testing sessions.
Create this folder when first needed.

---

## Workflow Rules

### After Building a New Proxy
1. The current d3d9.dll and proxy.ini at game root are the active test
2. Before deploying a new build, move the current d3d9.dll + proxy.ini + ffp_proxy.log into `Reverse/tests/YYYYMMDD-HHMMSS-<Yes|No>-<Description>/`

### After Testing
1. User reports result: path tracing rendered (Yes) or not (No)
2. Move active d3d9.dll + proxy.ini + ffp_proxy.log to `Reverse/tests/` with appropriate Yes/No label
3. Move any new runtime logs (metrics.txt, nrc_session_log.txt) to `Reverse/logs/remix-runtime/`

### Log Naming
- FFP proxy logs: keep original timestamped names
- DX trace captures: keep original `dxtrace_*` names
- Crash dumps in `.trex/`: leave in place (Remix manages these)

### What Never Stays at Game Root
- Old/backup proxy DLLs (`d3d9.proxy-pre-*.dll`, `d3d9.*.bak`)
- Old config backups (`proxy.pre-*.ini`, `user.prev-*.conf`)
- Old log files (anything with a timestamp in the name)
- Test snapshot folders
- Tracer DLLs not actively in use
