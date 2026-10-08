#!/usr/bin/env bash
# Refresh the local graph after source changes.
cd "$CLAUDE_PROJECT_DIR" || exit 0
command -v graphify >/dev/null 2>&1 || exit 0
[ -f graphify-out/graph.json ] || exit 0
git status --porcelain -- include src tests examples README.md CMakeLists.txt | grep -q . || exit 0
graphify update . >/dev/null 2>&1
exit 0
