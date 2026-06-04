#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MOVE_ANYTHING_SRC="${MOVE_ANYTHING_SRC:-${ROOT_DIR}/../schwung/src}"
BIN="$ROOT_DIR/build/tests/test_fork"

mkdir -p "$(dirname "$BIN")"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$MOVE_ANYTHING_SRC" \
  -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/test_fork.c" \
  -o "$BIN" \
  -lpthread

"$BIN"
