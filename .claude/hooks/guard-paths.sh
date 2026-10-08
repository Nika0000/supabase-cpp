#!/usr/bin/env bash
# PreToolUse Edit|Write: block edits to secrets and build output.
f=$(jq -r '.tool_input.file_path // empty')
rel=${f#"$CLAUDE_PROJECT_DIR"/}
if [[ "$rel" =~ ^(build[^/]*|vcpkg_installed)/ || "$rel" =~ (^|/)\.env ]]; then
  echo "Blocked: $rel is generated or secret." >&2
  exit 2
fi
exit 0
