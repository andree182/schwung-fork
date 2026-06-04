#!/usr/bin/env bash
# Build wrapper:
# - Uses scripts/build-local.sh when present (ignored in git, for personal setup)
# - Falls back to scripts/build-module.sh for reproducible/publish builds
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -f "$SCRIPT_DIR/build-local.sh" ]; then
    exec "$SCRIPT_DIR/build-local.sh" "$@"
fi

exec "$SCRIPT_DIR/build-module.sh" "$@"
