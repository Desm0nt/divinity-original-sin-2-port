#!/bin/bash
# Push everything Divinity Original Sin 2 Mobile needs to a phone without starting the game: patched modules (-> dm/out), the shader
# pack (-> dm/shaders), the RomFS (-> romfs: 47 files + TouchUI.pak, 11.4 GiB), the ASTC texture pack (-> astc, only with ASTC=1: phones
# without BC), the Turnip 27 adrenotools package (also bundled in the APK) and, with DOS2_INSTALL=1, the APK. Layout on the phone:
# /sdcard/Download/DivinityOriginalSin2Mobile/{romfs,dm,astc,drivers,logs}.
# Sources (defaults = the folder tools/setup_dos2.py prepares, work/DivinityOriginalSin2Mobile):
#   PREP=<dir> (default work/DivinityOriginalSin2Mobile) with romfs/, dm/out/, dm/shaders/, astc/; APK=<file> (default
#   work/DOS2-port-debug.apk); DRIVER=<dir> (default work/driver-assets/drivers/turnip27, from tools/fetch_turnip.py).
#   Device: ADB_SERIAL / ADB as in tools/run.sh.
# Big files: plain adb push first, tools/push_big.sh (resumable 256 MiB chunks) when it fails or the size is wrong; files already on
# the phone with the right size are skipped, so the script can simply be run again after an interruption. Never kills the adb server.
# (setup push [serial] does the same for the data folder without the APK / driver.)
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
export ADB_SERIAL=${ADB_SERIAL:-emulator-5554}
ADB="${ADB:-adb} -s $ADB_SERIAL"
R=/sdcard/Download/DivinityOriginalSin2Mobile
P=${PREP:-work/DivinityOriginalSin2Mobile}; APK=${APK:-work/DOS2-port-debug.apk}; DRV=${DRIVER:-work/driver-assets/drivers/turnip27}
rsize() { $ADB shell "stat -c %s '$1' 2>/dev/null" | tr -d '\r'; }
big() {   # <local> <remote>
  local want=$(stat -c %s "$1") have=$(rsize "$2")
  [ "$have" = "$want" ] && return
  if [ -n "$have" ] && [ "$have" -gt 0 ] 2>/dev/null && [ "$want" -gt 268435456 ]; then bash tools/push_big.sh "$1" "$2" 256 | tail -1; return; fi
  $ADB push "$1" "$2" 2>&1 | tail -1
  [ "$(rsize "$2")" = "$want" ] || { echo "plain push failed, chunks: $2"; $ADB shell "rm -f '$2'"; bash tools/push_big.sh "$1" "$2" 256 | tail -1; }
}
$ADB shell "mkdir -p $R/romfs $R/dm/out $R/dm/shaders $R/drivers $R/logs"
(cd "$P/dm/out" && $ADB push . $R/dm/out/ 2>&1 | tail -1)
big "$P/dm/shaders/shaders.xgspack" "$R/dm/shaders/shaders.xgspack"
[ -d "$DRV" ] && $ADB push "$DRV" $R/drivers/ 2>&1 | tail -1
[ -n "$DOS2_INSTALL" ] && [ -f "$APK" ] && $ADB install -r "$APK" 2>&1 | tail -1
[ -n "$DOS2_INSTALL" ] && $ADB shell appops set org.dos2.port MANAGE_EXTERNAL_STORAGE allow
for f in $(cd "$P/romfs" && find . -type f -printf '%s %P\n' | sort -n | cut -d' ' -f2-); do
  $ADB shell "mkdir -p '$R/romfs/$(dirname "$f")'" < /dev/null; big "$P/romfs/$f" "$R/romfs/$f" < /dev/null; done
if [ -n "$ASTC" ] && [ -f "$P/astc/dos2-astc.pack" ]; then
  $ADB shell "mkdir -p $R/astc"; big "$P/astc/dos2-astc.pack" "$R/astc/dos2-astc.pack"; $ADB push "$P/astc/manifest.json" $R/astc/ 2>&1 | tail -1; fi
echo "checks:"
$ADB shell "find $R/romfs -type f | wc -l; ls $R/dm/out | wc -l; ls -l $R/dm/shaders; du -sh $R"
echo "expected: $(cd "$P/romfs" && find . -type f | wc -l) romfs files, $(ls "$P/dm/out" | wc -l) out files"
