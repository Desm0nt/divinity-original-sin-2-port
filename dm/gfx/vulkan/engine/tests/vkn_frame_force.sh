#!/bin/bash
# [DOS2] VK-NATIVE [frame F1] the "on in every scenario" experiment (docs/dos2/nr/VK-FRAME-WORK.md section 7): vkn_stage3_test built with
# -DVKN_TEST_FORCE_FRAME=<bits> (vkn_pass.c: bit 1 forces DM_VK_NATIVE_LAZYCLR=1, bit 2 DM_VK_NATIVE_FRAMEAREA=1 into every policy the
# tests set), so every scenario of the suite (shadow cache, fused light, post, UI, visibility, the recording thread) runs under the piece.
# Expected: the checks that fail are the ones that count passes of the pass tests themselves and test_frame's own mode 0 / 2 checks;
# no VALIDATION line besides the validator's self-test, every journal / depth-content comparison of the other tests equal.
# Run from Git Bash:  MSYS_NO_PATHCONV=1 wsl bash /mnt/<drive>/.../dm/gfx/vulkan/engine/tests/vkn_frame_force.sh <1|2|3>
# Env: WT = the worktree (/mnt/... form; default: the tree this script lies in), I = a directory with only vulkan/ and vk_video/ of the NDK
# sysroot include (default /tmp/vkninc_force, filled from NDK).
set -u
BITS=${1:-3}
WT=${WT:-$(cd "$(dirname "$0")/../../../../.." && pwd)}
I=${I:-/tmp/vkninc_force}; mkdir -p $I
NDK=${NDK:?set NDK to <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include}
[ -d $I/vulkan ] || cp -r $NDK/vulkan $NDK/vk_video $I/
cd "$WT"
F="-std=c11 -D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_D3=1 -DDM_GAME_TL2=1 -DDM_GAME_DOS2=1 -O1 -g -w -I$I -DVKN_TEST_FORCE_FRAME=$BITS"
L=$(tr -d '\r' < dm/gfx/vulkan/engine/tests/vkn_stage3_test.c | sed -n '/wsl gcc/,/&& wsl/p' | tr ' ' '\n' | grep '^dm/.*\.c$' | sort -u | tr '\n' ' ')
O=/tmp/vkn_frame_force$BITS
gcc $F -o $O $L -lpthread -lm 2>&1 | tail -4
$O > $O.out 2>&1; echo "rc $? (1 is expected: the pass-count checks)"
echo "failing check lines (line(xcount)):"
grep -a "^FAIL" $O.out | awk -F: '{print $2}' | sort -n | uniq -c | awk '{printf "%s(x%s) ", $2, $1}'; echo
grep -a "checks" $O.out | tail -1
echo "VALIDATION lines (the self-test prints 4):"; grep -a -c "^VALIDATION" $O.out
