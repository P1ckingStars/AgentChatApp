#!/bin/sh
# Writes compile_commands.json (for clangd) from Bazel's C++ compile actions.
# Run from the repo root: bazel/compile_commands.sh
set -eu
cd "$(dirname "$0")/.."
bazel build //:ccbus //:ccbus-tui >/dev/null 2>&1 || true  # generated headers in external/
root=$(bazel info execution_root 2>/dev/null)
bazel aquery 'mnemonic("CppCompile", deps(//:ccbus) + deps(//:ccbus-tui))' \
    --output=jsonproto 2>/dev/null |
  jq --arg dir "$root" '[.actions[] | .arguments as $a
      | ($a | index("-c")) as $i | select($i != null)
      | {directory: $dir, file: $a[$i + 1], arguments: $a}
      | select(.file | startswith("external/") | not)] | unique_by(.file)' > compile_commands.json
echo "compile_commands.json: $(jq length compile_commands.json) files"
