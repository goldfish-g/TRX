#!/usr/bin/env bash
#
# TRX WebGL Engine Build Script
#
# Builds the TRX WASM engine + JS loader for web frontends.
#
# The engine build produces a self-contained artifact that any frontend
# (React, vanilla JS, TRCustoms, etc.) can consume via the adapter API
# defined in tools/shared/emscripten/trx-adapter.d.ts. Frontends are responsible
# for UI chrome, profile management, game-data uploads, and persistence
# policy.
#
# Prerequisites:
#   1. Install Emscripten SDK: https://emscripten.org/docs/getting_started/downloads.html
#      git clone https://github.com/emscripten-core/emsdk.git
#      cd emsdk && ./emsdk install latest && ./emsdk activate latest
#
#   2. Activate the SDK in your shell:
#      source /path/to/emsdk/emsdk_env.sh
#
#   3. Install Meson (>= 1.3.0) and Ninja:
#      pip install meson ninja
#
# Usage:
#   ./tools/build_webgl.sh [debug|release|debugoptim]
#
# Output (build/webgl/):
#   trx-adapter.js   Hand-written loader (public API, exports createTRX)
#   trx-adapter.d.ts TypeScript types for the adapter contract
#   trx-engine.js    Emscripten JavaScript glue (loaded by the adapter)
#   TRX.wasm         WebAssembly binary
#   TRX.data         Preloaded VFS: engine-authored ship assets
#                    (gameflow, catalogs, injections, shaders, images).
#                    NOT user game data — that is the frontend's concern.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CROSS_FILE="$PROJECT_ROOT/tools/shared/emscripten/emscripten_cross.ini"
SHARED_EMC="$PROJECT_ROOT/tools/shared/emscripten"

BUILD_TYPE="${1:-debug}"

echo "============================================"
echo "  TRX WebGL Engine Build"
echo "  Build type: $BUILD_TYPE"
echo "============================================"

if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found in PATH."
    echo "Please install and activate the Emscripten SDK first:"
    echo "  source /path/to/emsdk/emsdk_env.sh"
    exit 1
fi
echo "Using Emscripten: $(emcc --version | head -1)"

case "$BUILD_TYPE" in
    debug)      MESON_BUILDTYPE="debug" ;;
    release)    MESON_BUILDTYPE="release" ;;
    debugoptim) MESON_BUILDTYPE="debugoptimized" ;;
    *)
        echo "Unknown build type: $BUILD_TYPE (expected: debug, release, debugoptim)"
        exit 1
        ;;
esac

BUILD_DIR="$PROJECT_ROOT/build/webgl"
VFS_STAGE="$BUILD_DIR/vfs_stage"
DATA_ROOT="$PROJECT_ROOT/data"

# -----------------------------------------------------------------------
# Stage engine-authored ship assets into the VFS preload directory.
# -----------------------------------------------------------------------
echo ""
echo ">>> Staging engine ship data..."
rm -rf "$VFS_STAGE"
mkdir -p "$VFS_STAGE"

# Common config + shaders (always needed).
cp -rL "$DATA_ROOT/common/ship/cfg" "$VFS_STAGE/cfg"
cp -rL "$DATA_ROOT/common/ship/shaders" "$VFS_STAGE/cfg/shaders"

stage_base_game() {
    local engine="$1"  # tr1, tr2, tr3
    local mod="$2"     # game dir name (same as engine for base games)
    local src_cfg="$DATA_ROOT/$engine/ship/cfg"
    local src_data="$DATA_ROOT/$engine/ship/data"
    local dst="$VFS_STAGE/games/$mod"

    mkdir -p "$dst"

    if [ -d "$src_cfg/$mod" ]; then
        cp -L "$src_cfg/$mod"/*.json5 "$dst/" 2>/dev/null || true
    fi
    cp -L "$src_cfg"/catalog_*.csv "$dst/" 2>/dev/null || true
    cp -L "$src_cfg"/inv_ring.json5 "$dst/" 2>/dev/null || true
    cp -L "$src_cfg"/weapons.json5 "$dst/" 2>/dev/null || true

    if [ -d "$src_data/injections" ]; then
        cp -rL "$src_data/injections" "$dst/injections"
    fi
    if [ -d "$src_data/scripts" ]; then
        cp -rL "$src_data/scripts" "$dst/scripts"
    fi
    if [ -d "$src_data/images" ]; then
        cp -rL "$src_data/images" "$dst/images"
    fi
}

stage_expansion() {
    local engine="$1"
    local mod="$2"
    local src_cfg="$DATA_ROOT/$engine/ship/cfg"
    local dst="$VFS_STAGE/games/$mod"

    mkdir -p "$dst"
    if [ -d "$src_cfg/$mod" ]; then
        cp -L "$src_cfg/$mod"/*.json5 "$dst/" 2>/dev/null || true
    fi
}

stage_base_game tr1 tr1
stage_expansion tr1 tr1-ub
stage_expansion tr1 tr1-level

stage_base_game tr2 tr2
stage_expansion tr2 tr2-gm
stage_expansion tr2 tr2-level

stage_base_game tr3 tr3
stage_expansion tr3 tr3-la
stage_expansion tr3 tr3-level

# TRX-authored title/legal images live outside data/<engine>/ship/ in
# the per-game data directories.
for engine in tr1 tr2 tr3; do
    IMAGES_SRC="$PROJECT_ROOT/${engine}_data/data/images"
    IMAGES_DST="$VFS_STAGE/games/$engine/images"
    if [ -d "$IMAGES_SRC" ]; then
        mkdir -p "$IMAGES_DST"
        cp -u "$IMAGES_SRC"/* "$IMAGES_DST/" 2>/dev/null || true
    fi
done

# -----------------------------------------------------------------------
# Meson setup / configure / compile
# -----------------------------------------------------------------------
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo ""
    echo ">>> Configuring Meson build..."
    meson setup \
        --cross-file "$CROSS_FILE" \
        --buildtype "$MESON_BUILDTYPE" \
        -Dstaticdeps=false \
        -Dwebgl_vfs_stage="$VFS_STAGE" \
        "$BUILD_DIR" \
        "$PROJECT_ROOT/src/"
else
    echo ""
    echo ">>> Reconfiguring existing build..."
    meson configure \
        --buildtype "$MESON_BUILDTYPE" \
        -Dwebgl_vfs_stage="$VFS_STAGE" \
        "$BUILD_DIR"
fi

echo ""
echo ">>> Compiling..."
meson compile -C "$BUILD_DIR" TRX

# -----------------------------------------------------------------------
# Post-build: rename Emscripten JS, copy adapter + types.
# -----------------------------------------------------------------------
# Meson emits the Emscripten JS target as TRX.js. Rename to trx-engine.js
# so the case-sensitive pair (TRX.js vs trx.js) disappears. .wasm/.data
# sister files keep their original names; the adapter's locateFile hook
# remaps them at load time.
echo ""
echo ">>> Renaming TRX.js -> trx-engine.js..."
mv "$BUILD_DIR/TRX.js" "$BUILD_DIR/trx-engine.js"

echo ""
echo ">>> Copying adapter loader and types..."
cp "$SHARED_EMC/trx-adapter.js" "$BUILD_DIR/trx-adapter.js"
cp "$SHARED_EMC/trx-adapter.d.ts" "$BUILD_DIR/trx-adapter.d.ts"

echo ""
echo "============================================"
echo "  Build complete!"
echo ""
echo "  Output files:"
echo "    $BUILD_DIR/trx-adapter.js   (public loader, exports createTRX)"
echo "    $BUILD_DIR/trx-adapter.d.ts (TypeScript types for the adapter API)"
echo "    $BUILD_DIR/trx-engine.js    (Emscripten glue, internal)"
echo "    $BUILD_DIR/TRX.wasm         (WebAssembly binary)"
echo "    $BUILD_DIR/TRX.data         (preloaded VFS)"
echo ""
echo "  Drop these into your frontend's engine-version directory"
echo "  (see the trx-webgl-shell reference repo for an example)."
echo "============================================"
