#!/usr/bin/env bash
#
# TRX WebGL Build Script
#
# Builds TRX for the web using Emscripten (WASM + WebGL 2).
# Produces a single unified binary that supports all Tomb Raider games
# via a JavaScript profile selector.
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
#
# Usage:
#   ./tools/build_webgl.sh [debug|release|debugoptim] [--tr1] [--tr2] [--tr3] \
#                          [--ub] [--gm] [--la] [--eruda]
#
# Game data flags:
#   --tr1   Bundle Tomb Raider I game data (from tr1_data/)
#   --ub    Bundle Unfinished Business game data (from ub_data/)
#   --tr2   Bundle Tomb Raider II game data (from tr2_data/)
#   --gm    Bundle The Golden Mask game data (from gm_data/)
#   --tr3   Bundle Tomb Raider III game data (from tr3_data/)
#   --la    Bundle The Lost Artifact game data (from la_data/)
#   --eruda Inject eruda mobile debugger
#
# Output:
#   build/webgl/TRX.html
#   build/webgl/TRX.js
#   build/webgl/TRX.wasm
#   build/webgl/TRX.data  (preloaded config data)
#   build/webgl/<game>-gamedata.tar  (per-game data, fetched on demand)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CROSS_FILE="$PROJECT_ROOT/tools/shared/emscripten/emscripten_cross.ini"

BUILD_TYPE="${1:-debug}"
shift 1 2>/dev/null || true

# Game data flags
BUNDLE_TR1=false
BUNDLE_UB=false
BUNDLE_TR2=false
BUNDLE_GM=false
BUNDLE_TR3=false
BUNDLE_LA=false
ENABLE_ERUDA=false

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        debug|release|debugoptim) BUILD_TYPE="$arg" ;;
        --tr1)   BUNDLE_TR1=true ;;
        --ub)    BUNDLE_UB=true ;;
        --tr2)   BUNDLE_TR2=true ;;
        --gm)    BUNDLE_GM=true ;;
        --tr3)   BUNDLE_TR3=true ;;
        --la)    BUNDLE_LA=true ;;
        --eruda) ENABLE_ERUDA=true ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

BUILD_DIR="$PROJECT_ROOT/build/webgl"
VFS_STAGE="$BUILD_DIR/vfs_stage"
GAMEDATA_STAGE="$BUILD_DIR/gamedata_stage"
DATA_ROOT="$PROJECT_ROOT/data"

echo "============================================"
echo "  TRX WebGL Build (unified)"
echo "  Build type: $BUILD_TYPE"
echo "  Game data flags:"
$BUNDLE_TR1 && echo "    --tr1 (Tomb Raider I)"
$BUNDLE_UB  && echo "    --ub  (Unfinished Business)"
$BUNDLE_TR2 && echo "    --tr2 (Tomb Raider II)"
$BUNDLE_GM  && echo "    --gm  (The Golden Mask)"
$BUNDLE_TR3 && echo "    --tr3 (Tomb Raider III)"
$BUNDLE_LA  && echo "    --la  (The Lost Artifact)"
$BUNDLE_TR1 || $BUNDLE_UB || $BUNDLE_TR2 || $BUNDLE_GM || $BUNDLE_TR3 || $BUNDLE_LA || echo "    (none — upload-only build)"
echo "============================================"

# Verify Emscripten is available
if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found in PATH."
    echo "Please install and activate the Emscripten SDK first:"
    echo "  source /path/to/emsdk/emsdk_env.sh"
    exit 1
fi

echo "Using Emscripten: $(emcc --version | head -1)"

# Build type → Meson buildtype
case "$BUILD_TYPE" in
    debug)      MESON_BUILDTYPE="debug" ;;
    release)    MESON_BUILDTYPE="release" ;;
    debugoptim) MESON_BUILDTYPE="debugoptimized" ;;
    *)
        echo "Unknown build type: $BUILD_TYPE (expected: debug, release, debugoptim)"
        exit 1
        ;;
esac

# -----------------------------------------------------------------------
# VFS staging
# -----------------------------------------------------------------------
echo ""
echo ">>> Creating VFS staging directory..."
rm -rf "$VFS_STAGE"
mkdir -p "$VFS_STAGE"
rm -rf "$GAMEDATA_STAGE"
mkdir -p "$GAMEDATA_STAGE"

# --- Always staged: common config + shaders ---
echo ">>> Staging common config and shaders..."
cp -r "$DATA_ROOT/common/ship/cfg" "$VFS_STAGE/cfg"
cp -r "$DATA_ROOT/common/ship/shaders" "$VFS_STAGE/cfg/shaders"

# --- Always staged: per-engine ship config + data ---
# Each engine's base game and expansion dirs get gameflow, strings,
# catalogs, injections, scripts, and images.

stage_base_game() {
    local engine="$1"  # tr1, tr2, tr3
    local mod="$2"     # game dir name (same as engine for base games)
    local src_cfg="$DATA_ROOT/$engine/ship/cfg"
    local src_data="$DATA_ROOT/$engine/ship/data"
    local dst="$VFS_STAGE/games/$mod"

    mkdir -p "$dst"

    # Gameflow and strings from the mod's config subdir
    if [ -d "$src_cfg/$mod" ]; then
        cp "$src_cfg/$mod"/*.json5 "$dst/" 2>/dev/null || true
    fi

    # Catalogs, inv_ring, weapons from the engine config root
    cp "$src_cfg"/catalog_*.csv "$dst/" 2>/dev/null || true
    cp "$src_cfg"/inv_ring.json5 "$dst/" 2>/dev/null || true
    cp "$src_cfg"/weapons.json5 "$dst/" 2>/dev/null || true

    # Ship data: injections, scripts, images
    if [ -d "$src_data/injections" ]; then
        cp -r "$src_data/injections" "$dst/injections"
    fi
    if [ -d "$src_data/scripts" ]; then
        cp -r "$src_data/scripts" "$dst/scripts"
    fi
    if [ -d "$src_data/images" ]; then
        cp -r "$src_data/images" "$dst/images"
    fi
}

stage_expansion() {
    local engine="$1"  # tr1, tr2, tr3
    local mod="$2"     # expansion dir name (tr1-ub, tr2-gm, tr3-la)
    local src_cfg="$DATA_ROOT/$engine/ship/cfg"
    local dst="$VFS_STAGE/games/$mod"

    mkdir -p "$dst"

    # Expansion only needs gameflow + strings (shared data via base fallback)
    if [ -d "$src_cfg/$mod" ]; then
        cp "$src_cfg/$mod"/*.json5 "$dst/" 2>/dev/null || true
    fi
}

echo ">>> Staging TR1 ship data..."
stage_base_game tr1 tr1
stage_expansion tr1 tr1-ub
stage_expansion tr1 tr1-level

echo ">>> Staging TR2 ship data..."
stage_base_game tr2 tr2
stage_expansion tr2 tr2-gm
stage_expansion tr2 tr2-level

echo ">>> Staging TR3 ship data..."
stage_base_game tr3 tr3
stage_expansion tr3 tr3-la
stage_expansion tr3 tr3-level

# --- Copy TRX images into ship data ---
# TRX images (title screens, legal notices, credits) live in the game
# data directory but are TRX-authored assets.
for engine in tr1 tr2 tr3; do
    IMAGES_SRC="$PROJECT_ROOT/${engine}_data/data/images"
    IMAGES_DST="$VFS_STAGE/games/$engine/images"
    if [ -d "$IMAGES_SRC" ]; then
        echo ">>> Copying TRX images for $engine..."
        mkdir -p "$IMAGES_DST"
        cp -u "$IMAGES_SRC"/* "$IMAGES_DST/" 2>/dev/null || true
    fi
done

# --- Per-flag: bundle user game data ---
# Helper: stage user game data for a mod
stage_user_data() {
    local mod="$1"       # target dir name (tr1, tr1-ub, tr2, etc.)
    local data_dir="$2"  # source data dir (e.g. tr1_data)
    local level_ext="$3" # level file extension (.phd or .tr2)
    local dst="$GAMEDATA_STAGE/games/$mod"
    local src="$PROJECT_ROOT/$data_dir"

    if [ ! -d "$src" ]; then
        echo "WARNING: $data_dir/ not found, skipping user game data for $mod"
        return
    fi

    echo ">>> Bundling user game data for $mod from $data_dir..."

    # Level files (nocaseglob: original data uses uppercase extensions)
    if [ -d "$src/data" ]; then
        mkdir -p "$dst/levels"
        shopt -s nocaseglob
        for ext in $level_ext .psx .tub; do
            cp "$src/data"/*"$ext" "$dst/levels/" 2>/dev/null || true
        done
        # SFX files
        cp "$src/data"/*.sfx "$dst/" 2>/dev/null || true
        shopt -u nocaseglob
    fi

    # Music
    if [ -d "$src/music" ]; then
        mkdir -p "$dst/music"
        cp "$src/music"/* "$dst/music/" 2>/dev/null || true
    fi

    # FMV
    if [ -d "$src/fmv" ]; then
        mkdir -p "$dst/fmv"
        cp "$src/fmv"/* "$dst/fmv/" 2>/dev/null || true
    fi

    # Audio WAD (TR3)
    if [ -d "$src/audio" ]; then
        mkdir -p "$dst/audio"
        cp "$src/audio"/* "$dst/audio/" 2>/dev/null || true
    fi

    # Cuts (TR3)
    if [ -d "$src/cuts" ]; then
        mkdir -p "$dst/cuts"
        cp "$src/cuts"/* "$dst/cuts/" 2>/dev/null || true
    fi
}

$BUNDLE_TR1 && stage_user_data tr1 tr1_data ".phd"
$BUNDLE_UB  && stage_user_data tr1-ub ub_data ".phd"
$BUNDLE_TR2 && stage_user_data tr2 tr2_data ".tr2"
$BUNDLE_GM  && stage_user_data tr2-gm gm_data ".tr2"
$BUNDLE_TR3 && stage_user_data tr3 tr3_data ".tr2"
$BUNDLE_LA  && stage_user_data tr3-la la_data ".tr2"

# --- Generate profiles_defaults.json ---
echo ">>> Generating profiles_defaults.json..."
PROFILES_JSON="$VFS_STAGE/profiles_defaults.json"
echo '[' > "$PROFILES_JSON"
FIRST=true

add_default_profile() {
    local id="$1" name="$2" mod="$3" engine="$4" desc="$5"
    if $FIRST; then FIRST=false; else echo ',' >> "$PROFILES_JSON"; fi
    cat >> "$PROFILES_JSON" <<ENTRY
  {
    "id": "$id",
    "name": "$name",
    "mod": "$mod",
    "engine": $engine,
    "description": "$desc",
    "dataPackage": "${mod}-gamedata"
  }
ENTRY
}

$BUNDLE_TR1 && add_default_profile "tr1" "Tomb Raider I" "tr1" 1 \
    "The original adventure. Explore ancient ruins from Peru to Atlantis."
$BUNDLE_UB  && add_default_profile "tr1-ub" "Unfinished Business" "tr1-ub" 1 \
    "Bonus levels expanding Lara's first journey with new challenges."
$BUNDLE_TR2 && add_default_profile "tr2" "Tomb Raider II" "tr2" 2 \
    "From the Great Wall to the depths of the ocean."
$BUNDLE_GM  && add_default_profile "tr2-gm" "The Golden Mask" "tr2-gm" 2 \
    "A standalone arctic adventure hunting a legendary artifact."
$BUNDLE_TR3 && add_default_profile "tr3" "Tomb Raider III" "tr3" 3 \
    "A globe-spanning quest from India to Antarctica."
$BUNDLE_LA  && add_default_profile "tr3-la" "The Lost Artifact" "tr3-la" 3 \
    "Lara's return through the Scottish Highlands and London."

echo '' >> "$PROFILES_JSON"
echo ']' >> "$PROFILES_JSON"

# -----------------------------------------------------------------------
# Meson setup / configure
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
# Post-build: create per-game data packages
# -----------------------------------------------------------------------
BUNDLED_GAMES=()
$BUNDLE_TR1 && BUNDLED_GAMES+=(tr1)
$BUNDLE_UB  && BUNDLED_GAMES+=(tr1-ub)
$BUNDLE_TR2 && BUNDLED_GAMES+=(tr2)
$BUNDLE_GM  && BUNDLED_GAMES+=(tr2-gm)
$BUNDLE_TR3 && BUNDLED_GAMES+=(tr3)
$BUNDLE_LA  && BUNDLED_GAMES+=(tr3-la)

if [ ${#BUNDLED_GAMES[@]} -gt 0 ]; then
    echo ""
    echo ">>> Creating game data packages..."
    for game in "${BUNDLED_GAMES[@]}"; do
        if [ -d "$GAMEDATA_STAGE/games/$game" ]; then
            echo "    ${game}-gamedata.tar"
            tar cf "$BUILD_DIR/${game}-gamedata.tar" -C "$GAMEDATA_STAGE" "games/$game"
        fi
    done
fi

# -----------------------------------------------------------------------
# Post-build: copy support files
# -----------------------------------------------------------------------
SHARED_EMC="$PROJECT_ROOT/tools/shared/emscripten"
echo ""
echo ">>> Copying support files..."
cp "$SHARED_EMC/shell.css" "$BUILD_DIR/shell.css"
cp "$SHARED_EMC/gamedata.js" "$BUILD_DIR/gamedata.js"
cp "$SHARED_EMC/profiles.js" "$BUILD_DIR/profiles.js"
cp "$VFS_STAGE/profiles_defaults.json" "$BUILD_DIR/profiles_defaults.json"
mkdir -p "$BUILD_DIR/vendor"
cp "$SHARED_EMC/vendor/fflate.min.js" "$BUILD_DIR/vendor/fflate.min.js"

# Add cache-busting query strings to the built HTML so that browsers
# and reverse proxies (nginx, CDNs) never serve stale .js/.wasm/.data.
CACHE_BUST="v=$(cat "$BUILD_DIR/TRX.wasm" "$BUILD_DIR/TRX.js" | md5sum | cut -c1-8)"
echo ""
echo ">>> Cache-busting: $CACHE_BUST"
sed -i "s|src=\"TRX.js\"|src=\"TRX.js?${CACHE_BUST}\"|" "$BUILD_DIR/TRX.html"
sed -i "s|var _trxCacheBust = '';  // __CACHE_BUST__|var _trxCacheBust = '${CACHE_BUST}';|" "$BUILD_DIR/TRX.html"
sed -i "s|href=\"shell.css\"|href=\"shell.css?${CACHE_BUST}\"|" "$BUILD_DIR/TRX.html"
sed -i "s|src=\"gamedata.js\"|src=\"gamedata.js?${CACHE_BUST}\"|" "$BUILD_DIR/TRX.html"
sed -i "s|src=\"profiles.js\"|src=\"profiles.js?${CACHE_BUST}\"|" "$BUILD_DIR/TRX.html"
sed -i "s|src=\"vendor/fflate.min.js\"|src=\"vendor/fflate.min.js?${CACHE_BUST}\"|" "$BUILD_DIR/TRX.html"

# Inject eruda mobile debugger for test/debug builds.
if $ENABLE_ERUDA; then
    echo ""
    echo ">>> Injecting eruda mobile debugger..."
    ERUDA_TAGS='<script src="https:\/\/cdn.jsdelivr.net\/npm\/eruda"><\/script><script>eruda.init();<\/script>'
    sed -i "s|<!-- __ERUDA__ -->|${ERUDA_TAGS}|" "$BUILD_DIR/TRX.html"
fi

# --- PWA assets ---
APP_NAME="TRX"
SHORT_NAME="TRX"

echo ""
echo ">>> Generating PWA manifest..."
sed -e "s|__APP_NAME__|${APP_NAME}|g" \
    -e "s|__SHORT_NAME__|${SHORT_NAME}|g" \
    "$SHARED_EMC/manifest.webmanifest.in" > "$BUILD_DIR/manifest.webmanifest"

echo ">>> Generating service worker..."
sed -e "s|__APP_NAME__|${APP_NAME}|g" \
    -e "s|__SHORT_NAME__|${SHORT_NAME}|g" \
    -e "s|__CACHE_BUST__|${CACHE_BUST}|g" \
    "$SHARED_EMC/sw.js.in" > "$BUILD_DIR/sw.js"

echo ">>> Generating PWA icons..."
ICON_SRC="$PROJECT_ROOT/data/trx/icon.png"
python3 "$SHARED_EMC/generate_icons.py" "$ICON_SRC" "$BUILD_DIR"

echo ""
echo "============================================"
echo "  Build complete!"
echo ""
echo "  Output files:"
echo "    $BUILD_DIR/TRX.html"
echo "    $BUILD_DIR/TRX.js"
echo "    $BUILD_DIR/TRX.wasm"
echo ""
echo "  To test locally (COOP/COEP headers required for pthreads):"
echo "    cd $BUILD_DIR"
echo "    python3 -c \""
echo "import http.server"
echo "class H(http.server.SimpleHTTPRequestHandler):"
echo "    def end_headers(self):"
echo "        self.send_header('Cross-Origin-Opener-Policy','same-origin')"
echo "        self.send_header('Cross-Origin-Embedder-Policy','require-corp')"
echo "        super().end_headers()"
echo "http.server.HTTPServer(('',8080),H).serve_forever()\""
echo "    # Open http://localhost:8080/TRX.html"
echo "============================================"
