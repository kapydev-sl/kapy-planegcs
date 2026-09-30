#!/usr/bin/env bash
# services/planegcs/build/build-native.sh
#
# Builds the two planegcs artifacts without Docker, from pinned inputs, and
# prints the sha256 of each wasm:
#   planegcs.wasm       the embind build the app runs today
#   planegcs_capi.wasm  the C API (services/planegcs/build/capi), the build the
#                       app moves to (v3.3; the embind one goes away in S8)
# Nothing is installed globally: every input lives under $TOOLCHAIN, and the
# build runs in a scratch copy of this directory so the checkout stays clean.
# The pins below reproduce the committed planegcs.wasm byte for byte when the
# sources are unchanged (see ../README.md, "Rebuilding without Docker").
#
# Usage:
#   TOOLCHAIN=/path/with/space EMSDK=/path/to/emsdk NODE20=/path/to/node20/bin \
#       ./build-native.sh [--twice] [--publish]
#
# --twice    builds from scratch two times and fails unless both wasm files of
#            the two runs are byte for byte the same (the reproducibility gate).
# --publish  copies the result over ../dist/planegcs{,_capi}.{wasm,js}.
#
# WORK_DIR (optional) is the scratch directory; the default sits in $TOOLCHAIN.

set -euo pipefail

EMCC_VERSION="3.1.45"
EIGEN_URL="https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz"
EIGEN_SHA256="8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72"
BOOST_URL="http://archive.ubuntu.com/ubuntu/pool/main/b/boost1.74/libboost1.74-dev_1.74.0-14ubuntu3_amd64.deb"
BOOST_SHA256="4d9c90e43f0d25db6280d1ee326771cbb76462f73b9430f06bac1de8d05b7a78"

: "${TOOLCHAIN:?set TOOLCHAIN to a directory for the pinned inputs}"
: "${EMSDK:?set EMSDK to an emsdk checkout with ${EMCC_VERSION} installed}"
: "${NODE20:?set NODE20 to a Node 20 bin directory (tree-sitter-cpp does not build on Node >= 22)}"

HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$TOOLCHAIN"

fetch() { # url sha256 file
    if [ ! -f "$3" ]; then curl -sSLo "$3" "$1"; fi
    echo "$2  $3" | shasum -a 256 -c - >/dev/null
}

EIGEN_DIR="$TOOLCHAIN/eigen-3.4.0"
if [ ! -d "$EIGEN_DIR" ]; then
    fetch "$EIGEN_URL" "$EIGEN_SHA256" "$TOOLCHAIN/eigen-3.4.0.tar.gz"
    tar -xzf "$TOOLCHAIN/eigen-3.4.0.tar.gz" -C "$TOOLCHAIN"
fi
BOOST_DIR="$TOOLCHAIN/boost1.74-ubuntu3/usr/include"
if [ ! -d "$BOOST_DIR" ]; then
    fetch "$BOOST_URL" "$BOOST_SHA256" "$TOOLCHAIN/libboost1.74-dev.deb"
    mkdir -p "$TOOLCHAIN/boost1.74-ubuntu3"
    (cd "$TOOLCHAIN/boost1.74-ubuntu3" && ar x ../libboost1.74-dev.deb && tar -xf data.tar.*)
fi

# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
if ! emcc --version | head -1 | grep -q " ${EMCC_VERSION} "; then
    echo "emcc is not ${EMCC_VERSION}: $(emcc --version | head -1)" >&2
    exit 1
fi

WORK_BASE="${WORK_DIR:-$TOOLCHAIN/planegcs-work}"
ARTIFACTS=(planegcs.wasm planegcs_capi.wasm)

# Builds every artifact in the scratch directory $1.
build_once() {
    local WORK="$1"
    rm -rf "$WORK"
    mkdir -p "$WORK"
    (cd "$HERE" && tar --exclude node_modules --exclude planegcs/bin --exclude planegcs/CMakeFiles \
        --exclude planegcs/CMakeCache.txt -cf - .) | (cd "$WORK" && tar -xf -)

    (
        cd "$WORK"
        export PATH="$NODE20:$PATH"
        export npm_config_cache="$TOOLCHAIN/npm-cache"
        npm ci --no-audit --no-fund >/dev/null
        npm run build:bindings >/dev/null
    )

    (
        cd "$WORK/planegcs"
        emcmake cmake . -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            -DBoost_INCLUDE_DIR="$BOOST_DIR" -DEIGEN3_INCLUDE_DIR="$EIGEN_DIR" >/dev/null
        emmake make -j 8 >/dev/null
    )
}

shas() { # scratch dir -> "sha  name" lines
    local a
    for a in "${ARTIFACTS[@]}"; do
        (cd "$1/planegcs/bin" && shasum -a 256 "$a")
    done
}

if [ "${1:-}" = "--twice" ] || [ "${2:-}" = "--twice" ]; then
    build_once "$WORK_BASE-a"
    build_once "$WORK_BASE-b"
    SHAS_A="$(shas "$WORK_BASE-a")"
    SHAS_B="$(shas "$WORK_BASE-b")"
    echo "$SHAS_A"
    if [ "$SHAS_A" != "$SHAS_B" ]; then
        echo "NOT REPRODUCIBLE: the second build differs:" >&2
        echo "$SHAS_B" >&2
        exit 1
    fi
    echo "reproducible: two builds, same sha256 for ${#ARTIFACTS[@]} artifacts"
    WORK="$WORK_BASE-a"
else
    build_once "$WORK_BASE"
    shas "$WORK_BASE"
    WORK="$WORK_BASE"
fi

if [ "${1:-}" = "--publish" ] || [ "${2:-}" = "--publish" ]; then
    for a in "${ARTIFACTS[@]}"; do
        cp "$WORK/planegcs/bin/$a" "$HERE/../dist/$a"
        cp "$WORK/planegcs/bin/${a%.wasm}.js" "$HERE/../dist/${a%.wasm}.js"
    done
    echo "published to services/planegcs/dist/"
fi
