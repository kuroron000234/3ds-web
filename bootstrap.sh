#!/usr/bin/env bash
# /tmp/3ds に作業環境を再構築する（Cloud Shell は $HOME 以外が消えるため）。
# 使い方: ./bootstrap.sh          環境を作って CMake 構成まで行う
#         ./bootstrap.sh build    続けてビルドする
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-/tmp/3ds}"
AZAHAR_REV=6b4e7f2e694514fdf2f72dc20368609b7cd4912e
# ccache のキャッシュを再起動後も効かせるため、Emscripten の版を固定する
EMSDK_VERSION=7.0.0

# 必要なサブモジュールだけ取得する（dynarmic・Vulkan・Qt・SDL 関連は WASM では使わない）
SUBMODULES="externals/boost externals/nihstro externals/soundtouch externals/fmt externals/enet
externals/inih/inih externals/teakra externals/lodepng/lodepng externals/zstd externals/libyuv
externals/dds-ktx externals/faad2/faad2 externals/xxHash externals/libretro-common/libretro-common
externals/cryptopp externals/microtar externals/libressl dist/compatibility_list"

mkdir -p "$WORK/deps"
cd "$WORK"

if [ ! -d emsdk ]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git
    ./emsdk/emsdk install "$EMSDK_VERSION"
    ./emsdk/emsdk activate "$EMSDK_VERSION"
fi

if [ ! -d azahar ]; then
    git clone https://github.com/azahar-emu/azahar.git
    git -C azahar checkout -q "$AZAHAR_REV"
    # shellcheck disable=SC2086
    git -C azahar submodule update --init --depth 1 --jobs 4 $SUBMODULES
    git -C azahar apply "$HERE/patches/azahar.patch"
    git -C azahar/externals/libressl apply "$HERE/patches/libressl.patch"
fi

[ -d deps/robin-map ] || git clone --depth 1 https://github.com/Tessil/robin-map.git deps/robin-map

command -v ninja >/dev/null || pip install --user -q ninja

# shellcheck disable=SC1091
source emsdk/emsdk_env.sh >/dev/null 2>&1

# コンパイル結果を Home（永続領域）の ccache に溜め、再起動後の全体ビルドを短縮する。
# clang は再起動のたびに入れ直されて mtime が変わるので、版の文字列でコンパイラを識別する。
if command -v ccache >/dev/null; then
    export EM_COMPILER_WRAPPER="$(command -v ccache)"
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-3ds}"
    export CCACHE_COMPILERCHECK="string:emsdk-$EMSDK_VERSION"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-800M}"
fi

mkdir -p build
cd build
# -pthread: boost.asio とエミュレーター本体がスレッド必須 / -fwasm-exceptions: 例外を使う
# -Wno-c++11-narrowing: wasm32 は size_t が 32bit で、u64 からの縮小がエラーになる
FLAGS="-pthread -fwasm-exceptions -msimd128 -Wno-c++11-narrowing"
emcmake cmake ../azahar -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
    -DROBIN_MAP_INCLUDE_DIR="$WORK/deps/robin-map/include" \
    -DWEB_FRONTEND_DIR="$HERE/frontend" \
    -DENABLE_LIBRETRO=ON -DENABLE_QT=OFF -DENABLE_SDL2=OFF -DENABLE_TESTS=OFF \
    -DENABLE_ROOM=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_SCRIPTING=OFF -DENABLE_GDBSTUB=OFF \
    -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF \
    -DENABLE_VULKAN=OFF -DENABLE_OPENGL=ON -DENABLE_SOFTWARE_RENDERER=ON \
    -DENABLE_LTO=OFF -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF

if [ "${1:-}" = build ]; then
    ninja -k 0 azahar_web 2>&1 | tee "$WORK/build.log"
    command -v ccache >/dev/null && ccache --show-stats | head -8
    # /tmp は Cloud Shell の再起動で消えるので、成果物を Home 側にも残す
    cp bin/Release/azahar_web.js bin/Release/azahar_web.wasm "$HERE/web/"
fi
