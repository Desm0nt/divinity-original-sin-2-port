#!/bin/bash
# Divinity Original Sin 2 Mobile: one debug run on a device (ADB_SERIAL, default emulator-5554; ADB = the adb executable, default adb on PATH).
#   tools/run.sh <name> [seconds=40] [ENV=VALUE ...]
# Writes dm/env on the device (DM_DBG=1 + the given ENV lines), starts the app, waits, saves the full logcat to
# work/logs/<name>.log (host lines - dm / dm-vulkan / fatal signals - also to work/logs/<name>.dm.log), then force-stops the game.
# The screen must be on and unlocked: the game starts only when its surface exists. Rebuild + install first with
#   build.bat -Install <serial>     (or: pwsh tools/build_apk.ps1 -Install <serial>)
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
NAME=${1:?name}; SECS=${2:-40}; shift; [ $# -gt 0 ] && shift
SERIAL=${ADB_SERIAL:-emulator-5554}
ADB="${ADB:-adb} -s $SERIAL"
PKG=org.dos2.port
D=/sdcard/Download/DivinityOriginalSin2Mobile/dm
STOP='CRASH|FATAL|die:|Fatal signal'
mkdir -p work/logs
{ echo "DM_DBG=1"; for kv in "$@"; do echo "$kv"; done; } > work/logs/$NAME.env
$ADB push work/logs/$NAME.env "$D/env" > /dev/null || { echo "push env failed ($SERIAL)"; exit 1; }
$ADB shell input keyevent KEYCODE_WAKEUP
$ADB shell am force-stop $PKG
$ADB logcat -G 64M >/dev/null 2>&1; $ADB logcat -c
echo "[$SERIAL] $NAME: start, $SECS s, env: $*"
$ADB shell am start -n $PKG/com.xiaoboluo.d2r.DmActivity > /dev/null 2>&1
end=$((SECONDS + SECS))
pid0=""; for i in 1 2 3 4 5 6 7 8 9 10; do pid0=$($ADB shell pidof $PKG | tr -d '
'); [ -n "$pid0" ] && break; sleep 1; done
while [ $SECONDS -lt $end ]; do
  sleep 3
  $ADB logcat -d -s dm:* dm-vulkan:* libc:F 2>/dev/null | grep -Eq "$STOP" && { sleep 3; break; }
done
# no crash: make the app dump every guest thread (our SIGSEGV handler) before stopping it -- shows where a hang waits
if ! $ADB logcat -d -s dm:* dm-vulkan:* libc:F 2>/dev/null | grep -Eq "$STOP"; then
  pid=$($ADB shell pidof $PKG | tr -d '\r')
  [ -n "$pid" ] && [ -n "${DUMP_THREADS:-}" ] && { $ADB shell run-as $PKG kill -11 $pid; sleep 3; }   # [DOS2] only with DUMP_THREADS=1 (a false crash.txt otherwise)
fi
$ADB logcat -d > work/logs/$NAME.log
# 15.11 (run t3k): after a crash the owner restarted the game by hand and this force-stop killed it; stop only the process this run started
cur=$($ADB shell pidof $PKG | tr -d '
')
if [ -n "$cur" ] && [ "$cur" = "$pid0" ]; then $ADB shell am force-stop $PKG; else echo "not stopping: the game now is pid '${cur}', this run started '${pid0}'"; fi
grep -E "\[dm\]|\[guest\]| dm-vulkan:|Fatal signal" work/logs/$NAME.log > work/logs/$NAME.dm.log
echo "log: work/logs/$NAME.log ($(wc -l < work/logs/$NAME.dm.log) host lines)"
grep -m1 -E -A8 "CRASH|FATAL|Break#|Fatal signal" work/logs/$NAME.dm.log
tail -n 5 work/logs/$NAME.dm.log
