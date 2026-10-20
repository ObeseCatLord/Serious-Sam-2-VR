#!/bin/bash
# Builds the 32-bit OpenAL Soft DLL the spatial audio loads (Bin/SS2VR/OpenAL32.dll)
# from the official 1.24.3 release source into build-openal/.
# Source: https://github.com/kcat/openal-soft/archive/refs/tags/1.24.3.tar.gz
# (sha256 7e1fecdeb45e7f78722b776c5cf30bd33934b961d7fd2a11e0494e064cc631ce).
# Usage: tools/build_openal.sh <extracted openal-soft-1.24.3 directory>
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?openal-soft source directory}
cmake -S "$SRC" -B "$ROOT/build-openal/work" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw32.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DLIBTYPE=SHARED -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL=OFF \
  -DALSOFT_BACKEND_PIPEWIRE=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_PORTAUDIO=OFF \
  -DALSOFT_BACKEND_SDL2=OFF -DALSOFT_BACKEND_WASAPI=ON -DALSOFT_BACKEND_DSOUND=ON -DALSOFT_BACKEND_WINMM=ON \
  -DALSOFT_EMBED_HRTF_DATA=ON -DCMAKE_SHARED_LINKER_FLAGS="-static -static-libgcc -static-libstdc++" \
  -DCMAKE_CXX_FLAGS="-faligned-new=8" # mingw32 malloc gives 8-byte alignment; GCC otherwise assumes 16
cmake --build "$ROOT/build-openal/work" -j"$(nproc)"
i686-w64-mingw32-strip -o "$ROOT/build-openal/OpenAL32.dll" "$ROOT/build-openal/work/OpenAL32.dll"
echo "$ROOT/build-openal/OpenAL32.dll"
