#!/bin/sh
# usage: nvk-source.sh <mesa checkout> [<nvk-build-id>]: the lines naming what an NVK dylib's pipeline cache depends on, which
# //gpu:seed (tools/gpu/seed.py) compares between the iOS dylib and its host twin (<dylib>.source beside each): the git tree id of the
# checkout's files as they are (tracked, and untracked ones .gitignore keeps), whatever commits or patches made them, and the
# -Dnvk-build-id the build was configured with (empty: the pipeline cache UUID hashes the dylib's own LC_UUID). Uses a temporary
# index only.
set -e
INDEX=$(mktemp -t nvk-source-index)
trap 'rm -f "$INDEX"' EXIT
export GIT_INDEX_FILE=$INDEX
git -C "$1" read-tree HEAD
git -C "$1" add -A
echo "mesa-tree $(git -C "$1" write-tree)"
echo "nvk-build-id ${2:-}"
