#!/usr/bin/env bash
# PostToolUse Edit|Write: clang-format touched sources. No-op if clang-format is missing.
command -v clang-format >/dev/null 2>&1 || exit 0
f=$(jq -r '.tool_response.filePath // .tool_input.file_path // empty')
rel=${f#"$CLAUDE_PROJECT_DIR"/}
[[ "$rel" =~ ^(include|src|tests|examples)/.*\.(c|cpp|h|hpp)$ ]] || exit 0
clang-format -i "$f"
