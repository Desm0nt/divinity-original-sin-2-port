#!/usr/bin/env bash
# N1 (docs/dos2/nr/NR-NSHADER.md): build the OFFLINE Adreno compiler setup in WSL (Ubuntu-22.04, no root):
#   1. a host (x86_64 Linux) build of the same Mesa tree as the phone's turnip27 (<tools>/src/mesa-full, Mesa main 1c3a936), Turnip with
#      the kgsl kmd only -> $HOME/n1mesa-build/src/freedreno/vulkan/libvulkan_freedreno.so
#   2. tu_stats + fakekgsl.so (a fake /dev/kgsl-3d0: chip 0x43051401 = FD750) + phone_chain -> $HOME/n1tools
# Uses the toolchain of doom-eternal-port/tools/turnip_build (<tools>/turnip-env: meson / ninja / mako venv, bison / flex / pkg-config
# debs, glslang). Nothing is downloaded. zlib: the host has only libz.so.1, so the NDK's zlib.h + a pkg-config file point at it.
# Host-only source patch (the copy in $HOME/n1mesa, never the original tree): libstdc++ 11 cannot hold the incomplete tu_autotune::rp_history
# in std::unordered_map -> the member becomes a unique_ptr (autotune is never used by tu_stats).
#   wsl -d Ubuntu-22.04 -- bash <tree>/tools/tu_stats/build_host.sh [tools-only]      (TOOLS=<dir with turnip-env, src/mesa-full, NDK>)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ENGINE=$(cd "$HERE/../../dm/gfx/vulkan/engine" && pwd)
TOOLS=${TOOLS:?set TOOLS to the folder with turnip-env, src/mesa-full and the NDK}   # as doom-eternal-port/tools/turnip_build/build_turnip.sh
ENVD=$TOOLS/turnip-env
NDK_INC=$TOOLS/android-ndk-linux/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
SRC=$HOME/n1mesa B=$HOME/n1mesa-build O=$HOME/n1tools Z=$HOME/n1zlib
mkdir -p "$O"
if [ "${1:-}" != tools-only ]; then
    [ -f "$SRC/meson.build" ] || { mkdir -p "$SRC"; (cd "$TOOLS/src/mesa-full" && tar --exclude=./.git -cf - .) | (cd "$SRC" && tar -xf -); }
    if ! grep -q rp_histories_up "$SRC/src/freedreno/vulkan/tu_autotune.h"; then
        sed -i 's/^   rp_histories_t rp_histories;/   std::unique_ptr<rp_histories_t> rp_histories_up{}; \/* N1 host build: libstdc++ 11 needs a complete mapped type *\//' "$SRC/src/freedreno/vulkan/tu_autotune.h"
        sed -i '0,/^#include "tu_autotune.h"/s//#include "tu_autotune.h"\n#define rp_histories (*rp_histories_up)/' "$SRC/src/freedreno/vulkan/tu_autotune.cc"
        awk 'BEGIN{d=0} {print} /^tu_autotune::tu_autotune\(struct tu_device \*device, VkResult &result\)/{d=1} d==1 && /^{/ {print "   rp_histories_up.reset(new rp_histories_t());"; d=2}' \
            "$SRC/src/freedreno/vulkan/tu_autotune.cc" > "$SRC/t.cc" && mv "$SRC/t.cc" "$SRC/src/freedreno/vulkan/tu_autotune.cc"
    fi
    mkdir -p "$Z/include" "$Z/lib/pkgconfig"
    cp "$NDK_INC/zlib.h" "$NDK_INC/zconf.h" "$Z/include/"; ln -sf /usr/lib/x86_64-linux-gnu/libz.so.1 "$Z/lib/libz.so"
    printf 'prefix=%s\nName: zlib\nDescription: zlib (NDK headers, host libz.so.1)\nVersion: 1.2.11\nLibs: -L%s/lib -lz\nCflags: -I%s/include\n' "$Z" "$Z" "$Z" > "$Z/lib/pkgconfig/zlib.pc"
    export PATH="$ENVD/venv/bin:$ENVD/root/usr/bin:$ENVD/glslang/bin:$PATH"
    export BISON_PKGDATADIR="$ENVD/root/usr/share/bison" M4="$ENVD/root/usr/bin/m4"
    export LD_LIBRARY_PATH="$ENVD/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export MESA_GIT_SHA1_OVERRIDE=1c3a936490 CFLAGS="-Wno-error" CXXFLAGS="-Wno-error" PKG_CONFIG_PATH="$Z/lib/pkgconfig"
    [ -f "$B/build.ninja" ] || meson setup "$B" "$SRC" -Dbuildtype=debugoptimized -Dplatforms= -Dvideo-codecs= -Dgallium-drivers= \
        -Dvulkan-drivers=freedreno -Dvulkan-beta=true -Dfreedreno-kmds=kgsl -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dxmlconfig=disabled -Dzlib=enabled -Dzstd=disabled -Dexpat=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dshader-cache=disabled -Dbuild-tests=false
    ninja -C "$B" src/freedreno/vulkan/libvulkan_freedreno.so
fi
gcc -O1 -fPIC -shared -o "$O/fakekgsl.so" "$HERE/fakekgsl.c" -I"$SRC/src/freedreno/vulkan" -ldl
g++ -O1 -std=c++17 -o "$O/tu_stats" "$HERE/tu_stats.cpp" -I"$SRC/include" -ldl
g++ -O2 -std=c++17 -o "$O/phone_chain" "$HERE/phone_chain.cpp" "$ENGINE/xbg_spirv_ldc.cpp" "$ENGINE/xbg_spirv_temps.cpp" \
    "$ENGINE/xbg_spirv_peep.cpp" "$ENGINE/xbg_spirv_ubovec.cpp" -I"$ENGINE"
ls -la "$B/src/freedreno/vulkan/libvulkan_freedreno.so" "$O"
