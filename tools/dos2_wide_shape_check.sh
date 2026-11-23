#!/usr/bin/env sh
# [DOS2] issue #9 host check: the native window decision (dm/host/hle_dos2_wide_size.h, its test dm/host/tests/dos2_wide_size_test.c) and the
# Java front end's mirror (dm/android/.../Dos2Shape.java, printed by dm/host/tests/Dos2ShapeTable.java) must agree case by case.
# Needs a C compiler (CC, default cc; clang works) and a JDK (JAVA_HOME first: the Windows javapath shim crashes under MSYS; else PATH).
# Builds into a temporary directory outside the repo.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
JB=${JAVA_HOME:+$JAVA_HOME/bin/}
"$CC" -D_CRT_SECURE_NO_WARNINGS -O1 "$root/dm/host/tests/dos2_wide_size_test.c" -lm -o "$tmp/wst" 2>/dev/null \
  || "$CC" -D_CRT_SECURE_NO_WARNINGS -O1 "$root/dm/host/tests/dos2_wide_size_test.c" -o "$tmp/wst"
"$tmp/wst"
"$tmp/wst" --table | tr -d '\r' > "$tmp/c.txt"
"${JB}javac" -d "$tmp/j" "$root/dm/android/app/src/main/java/com/xiaoboluo/d2r/Dos2Shape.java" "$root/dm/host/tests/Dos2ShapeTable.java"
"${JB}java" -cp "$tmp/j" com.xiaoboluo.d2r.Dos2ShapeTable | tr -d '\r' > "$tmp/java.txt"
if diff "$tmp/c.txt" "$tmp/java.txt" > "$tmp/diff.txt"; then
  echo "PASS: native and Java agree on $(wc -l < "$tmp/c.txt" | tr -d ' ') cases"
else
  echo "FAIL: native (<) and Java (>) differ:"; cat "$tmp/diff.txt"; exit 1
fi
