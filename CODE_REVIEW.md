# Code Review Checklist

Before merging, verify the following:

- **General Purpose x86/x64 RE Tools**: The project is meant for general purpose x86/x64 RE tools. It is ok to sometimes only support x86 or x64 but always encouraged to try implementing your tool for both, and in a way that is general purpose, not game-specific with LLM friendly outputs, Unix-like tools that can be combined together. We focus on Windows for now, so Linux/MacOs can be omited.
- **Legal & Scope**: No game-specific or app-specific data (function maps, address databases, struct definitions) in tracked core code. General-purpose signatures only (CRT, compiler, STL). Project-specific data goes in gitignored workspace directories or a separate package that can be pulled from the web, not MIT-licensed under our code.
- **No Duplication**: When adding a new tool, always check if one doesn't already exist, and if so, make a smart decision on how to expand/add to it so its current functionalities still work and we adhere to the "One obvious way of doing things" rule, while keeping the tools general purpose.
- **IDEs Instructions in Sync**: Always review all IDEs rules/instructions/hooks to make sure they all contain the descriptions of all tools and are generally kept in sync so all users can have the same experience despite the IDE they use.
- **Apply Repo Rules**: We contribute to IDE specific rules/skills/hooks/instructions files like .cursor, .claude, .github or .kiro so LLMs can write educated code from the beginning, but this often fails (context window, copium quick solutions, etc.), so aways do a pass of the code changes (preferably diff with merge-base, asking user what is the base branch it wants to merge to) against all our rules.
