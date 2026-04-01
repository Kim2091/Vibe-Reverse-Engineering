---
description: Subagent delegation rules — when to spawn static-analyzer vs run livetools directly, parallel work patterns
---

# Subagent Workflow

Main agent: **live tools**, **dx9tracer capture**, **user interaction**, **synthesis**. Heavy static analysis and web research → subagents.

## Pre-flight: Ensure Ghidra Backend

Before first pyghidra use, run `python verify_install.py` — if pyghidra shows WARN, run `python verify_install.py --setup` (one-time ~600MB download).

## Bootstrap First — New Binaries

When analyzing a binary for the first time (no existing or sparsely populated `patches/<project>/kb.h`), **always bootstrap before other static analysis**:

1. Spawn `static-analyzer`: `bootstrap.py <binary> --project <Name>` — seeds kb.h with RTTI, CRT/library IDs, compiler info, propagated labels. **2-5 minutes.** After bootstrap, ALL `decompiler.py` calls must use `--types patches/<project>/kb.h`.
2. **In parallel**, spawn second `static-analyzer`: `pyghidra_backend.py analyze <binary> --project patches/<Name>` — full Ghidra analysis, reusable project. **5-15 minutes.** After this, use `--project patches/<project>` so `--backend auto` prefers Ghidra.
3. Other static analysis can run in parallel but output is richer after bootstrap.

**Detect "needs bootstrap":** `grep -cE '^[@$]|^struct |^enum ' patches/<project>/kb.h` — count under 50 = bootstrap.
**Detect "needs pyghidra analyze":** Check if `patches/<project>/ghidra/<binary_stem>.gpr` exists.

## Delegation Beyond CLAUDE.md

CLAUDE.md lists allowlisted fast commands (run directly) and the general delegation rule. Additional non-obvious delegation:

| Task | Where | Notes |
|------|-------|-------|
| Web research (docs, API refs, specs) | `web-researcher` subagent | |
| dx9tracer offline analysis | `static-analyzer` subagent | |
| Subsequent Ghidra decompile | `static-analyzer` subagent | Fast: JVM ~3s + decompile <1s |
| sigdb scan / build | `static-analyzer` subagent | scan 1-3 min, build 1-5 min |
| KB updates from findings | `static-analyzer` writes kb.h | main agent may refine |

## Subagent Output

Subagents write to `patches/<project>/findings.md` (appended). When a subagent returns, **read the file** for full details — the return message is just a summary.

## Parallel Work

1. Spawn `static-analyzer` **in background** for static questions
2. **Immediately** ask user to launch the game — don't wait for static results
3. While subagent works, prepare livetools or discuss approach
4. Synthesize when subagent returns

Multiple `static-analyzer` instances can run in parallel for independent questions. When results have multiple leads, spawn parallel subagents — don't serialize.

## Dual-Backend Deep Analysis

For complex exploratory tasks (finding subsystems, mapping pipelines), spawn **two parallel agents**:

1. **r2ghidra**: `--backend pdg --types kb.h` → writes `findings_r2.md`
2. **pyghidra**: `pyghidra_backend.py decompile` → writes `findings.md`

r2ghidra: better `__thiscall` recovery, low-level D3D. pyghidra: better library call resolution, type propagation. Merge both for complete picture. Not needed for single-function decompilation — use `--backend auto`.

## Main Agent During Analysis

**Do not silently wait.** While static analysis runs:
- Ask user to launch game if live verification/patching needed
- Prepare livetools commands from what you already know
- Discuss the approach

## Examples

**"Analyze game.exe for the first time"**
1. Background: `bootstrap.py game.exe --project MyGame`
2. Background: `pyghidra_backend.py analyze game.exe --project patches/MyGame`
3. Tell user, run `sigdb.py fingerprint` inline while waiting
4. When both return, all subsequent decompilations use `--types kb.h --project patches/MyGame`

**"Disable culling in game.exe"**
1. Spawn two static-analyzers (r2ghidra + pyghidra): find `SetRenderState` with `D3DRS_CULLMODE`, string search "cull"
2. Tell user: "Launch the game — I'll patch culling live once I find addresses"
3. Merge findings → `livetools mem write` to NOP cull-enable or force `D3DCULL_NONE`

## Anti-Patterns

- **Cascade Trap**: Running "one quick xref" then chasing callers until you're doing full static analysis while user waits. Second retools command = should have delegated.
- **Duplicating subagent work**: Don't grep for the same thing you delegated. Trust the subagent.
- **Silent waiting**: Always talk to user or do livetools while subagents run.
