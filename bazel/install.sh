#!/bin/sh
# Copies the built binaries into PREFIX/bin (default ~/.local). Hooks registered by
# `ccbus install-hooks` point at the installed ccbus, not at bazel's output tree.
set -eu
ccbus=$1 tui=$2
prefix=${3:-$HOME/.local}
mkdir -p "$prefix/bin"
install -m 755 "$ccbus" "$prefix/bin/ccbus"
install -m 755 "$tui" "$prefix/bin/ccbus-tui"
echo "installed ccbus and ccbus-tui to $prefix/bin"
