#!/bin/bash
# Run the tree's XNU build script with exactly the given XNU options:
# no XNU_* variable from the caller's environment, LLVM's tools first on PATH.
# Usage: xnu-make.sh <tool dirs to prepend to PATH> <script> [NAME=VALUE...]
set -euo pipefail
tools=$1 script=$2
shift 2
for name in $(compgen -e); do
  case $name in XNU_*) unset "$name" ;; esac
done
export PATH="$tools:$PATH"
for option in "$@"; do
  export "$option"
done
exec bash "$script"
