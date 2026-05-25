#!/usr/bin/env bash
# Docker entrypoint for WebGL/Emscripten builds.
#
# Usage:
#   docker run ... rrdash/trx-webgl build --target release --tr1 --tr2
set -euo pipefail

# Allow git operations on the volume-mounted repo (different UID).
git config --global --add safe.directory /app

ACTION="${1:-build}"
shift || true

TARGET="debug"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)   TARGET="$2"; shift 2 ;;
        --tr1)      EXTRA_ARGS+=("--tr1"); shift ;;
        --ub)       EXTRA_ARGS+=("--ub"); shift ;;
        --tr2)      EXTRA_ARGS+=("--tr2"); shift ;;
        --gm)       EXTRA_ARGS+=("--gm"); shift ;;
        --tr3)      EXTRA_ARGS+=("--tr3"); shift ;;
        --la)       EXTRA_ARGS+=("--la"); shift ;;
        --eruda)    EXTRA_ARGS+=("--eruda"); shift ;;
        *)          shift ;;
    esac
done

case "$ACTION" in
    build)
        exec /app/tools/build_webgl.sh "$TARGET" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    *)
        echo "Unknown action: $ACTION (expected: build)"
        exit 1
        ;;
esac
