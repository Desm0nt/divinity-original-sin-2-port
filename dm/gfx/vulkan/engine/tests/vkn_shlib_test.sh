#!/bin/bash
# [DOS2] VK-NATIVE shader library comparison test (vkn_shlib_test.cpp): build under WSL and run it for every device model.
# Run from Git Bash:  MSYS_NO_PATHCONV=1 wsl bash <checkout>/dm/gfx/vulkan/engine/tests/vkn_shlib_test.sh [limit] [models...]
# Env: WT = the worktree (WSL path form), B = the build / copy directory (default ~/vkn-shlib-test; one per parallel run), SHADERS = the shader directory (default <dos2-port>/work/shaders-vk), NATIVE = a native.xgspack
# (default <dos2-port>/work/shaders-native/native.xgspack), NR = a native-nr.xgspack for the "+nr" models (stage 5 A1; default
# <dos2-port>/work/native-lightfuse/native-nr.xgspack, the newest full one: x / m / d of all 16 NR-04 entries). The tree is copied into the Linux file system first: gcc's #pragma once is not
# reliable on /mnt drives, and the 270 MB pack reads much faster there.
set -u
WT=${WT:?set WT to the checkout (WSL path)}
PORT=$(dirname "$WT")
SHADERS=${SHADERS:-$PORT/work/shaders-vk}
NATIVE=${NATIVE:-$PORT/work/shaders-native/native.xgspack}
NR=${NR:-$PORT/work/native-lightfuse/native-nr.xgspack}
LIMIT=${1:-0}; shift || true
# "+nr<q>" (q 0..3), "+nrv", "+nr3nl": the named-bundle replacement on both sides (vkn_shlib_test.cpp head comment)
MODELS=${*:-adreno mali powervr adreno-simmali adreno-native adreno-files adreno+nr3 mali+nr3 powervr+nr3 adreno+nr1 adreno+nr2 adreno+nrv adreno+nr3nl adreno-native+nr3}
NDK=${NDK:?set NDK to <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include}
B=${B:-$HOME/vkn-shlib-test}; mkdir -p $B/inc/android $B/obj $B/shaders $B/src/dm
[ -d $B/inc/vulkan ] || cp -r $NDK/vulkan $NDK/vk_video $B/inc/
[ -f $B/inc/zlib.h ] || cp $NDK/zlib.h $NDK/zconf.h $B/inc/
cat > $B/inc/android/hardware_buffer.h <<'EOF'
// host shim of the NDK header: only what xbg_game.cpp names (never called by this test)
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
enum { AHARDWAREBUFFER_FORMAT_BLOB = 0x21 };
enum { AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3ULL, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN = 3ULL << 4, AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER = 1ULL << 24 };
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int socketFd);
#ifdef __cplusplus
}
#endif
EOF
rsync -a --delete --exclude ios "$WT/dm/" $B/src/dm/
rsync -a "$SHADERS/" $B/shaders/
[ -f "$NATIVE" ] && cp -u "$NATIVE" $B/shaders/native.xgspack
[ -f "$NR" ] && cp -u "$NR" $B/shaders/native-nr.xgspack
cd $B/src
D="-D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_D3=1 -DDM_GAME_TL2=1 -DDM_GAME_DOS2=1 -DVK_USE_PLATFORM_ANDROID_KHR -DDM_ASTC_LAZY=1 -O1 -w -I$B/inc -Idm/gfx/vulkan -Idm/gfx/nvn/include -Idm/host -Idm"
OBJS=""
for f in dm/gfx/vulkan/engine/*.cpp dm/gfx/vulkan/engine/*.c dm/gfx/vkn/vkn_shlib.cpp dm/gfx/vkn/vkn_shlib_named.cpp dm/gfx/nr/nr_ubodyn.c dm/gfx/nr/nr_sh.c dm/gfx/vulkan/engine/tests/vkn_shlib_test.cpp; do
  b=$(basename $f); case $b in xbg_spvopt.cpp|xbg_astc_lazy.cpp) continue;; esac
  o=$B/obj/$b.o; OBJS="$OBJS $o"
  [ $o -nt $f ] && continue
  if [[ $f == *.cpp ]]; then g++ -std=c++17 $D -c $f -o $o || exit 2; else gcc -std=gnu11 $D -c $f -o $o || exit 2; fi
done
g++ -no-pie -o $B/vkn_shlib_test $OBJS -Wl,--unresolved-symbols=ignore-all -lpthread -ldl -l:libz.so.1 || exit 2
rc=0
for m in $MODELS; do
  $B/vkn_shlib_test $m $B/shaders $LIMIT > $B/out_$m.txt 2>&1; r=$?
  grep -v "^DIFF" $B/out_$m.txt | tail -4
  [ $r -ne 0 ] && { rc=1; grep "^DIFF\|FATAL\|signal\|symbol" $B/out_$m.txt | head -20; }
done
echo "vkn_shlib_test rc $rc"
exit $rc
