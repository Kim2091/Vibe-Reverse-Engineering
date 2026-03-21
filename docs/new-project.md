---
name: new-project
description: Scaffold a new RE project under patches/
argument-hint: "<project-name>"
allowed-tools: ["Bash", "Write", "Read", "Glob"]
---

Create a new reverse engineering project directory under `patches/`.

## Input

The user provides a project name (e.g., "cool_game", "timeline", "mb_warband").

## Process

1. Verify `patches/<project>/` doesn't already exist. If it does, tell the user and stop.

2. Create the directory structure:
   ```
   patches/<project>/
   ├── kb.h           # Knowledge base (empty template)
   └── traces/        # For JSONL trace data (livetools, dx9 tracer)
   ```

3. Write `patches/<project>/kb.h` with this template:
   ```c
   // Knowledge base for <project>
   //
   // Format:
   //   struct/enum definitions    — C type definitions
   //   @ 0xADDR signature         — function at address
   //   $ 0xADDR Type g_name       — global variable at address
   ```

4. Create the empty `traces/` directory (use `mkdir -p`).

5. Report what was created and suggest next steps:
   - Run analysis scripts if this is an FFP porting project
   - Start with `search.py strings` to find entry points
   - Use `/kb-update <project>` to add findings

$ARGUMENTS
