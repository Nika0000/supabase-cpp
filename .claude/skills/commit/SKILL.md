---
name: commit
description: Format touched files, then commit with a conventional message.
---
1. `rtk git status`, `rtk git diff --stat`.
2. `clang-format -i` only on changed C++ files.
3. Commit with `feat:`/`fix:`/`docs:`/`style:`/`test:`/`chore:`. Never push.
