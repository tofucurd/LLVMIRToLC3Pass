#!/bin/sh
# run_c_test.sh <test_name> <clang> <opt> <plugin_so> <src_dir>
# Compiles a .c test to .ll, then runs the LC-3 pass.
set -e
NAME="$1"
CLANG="$2"
OPT="$3"
PLUGIN="$4"
SRC_DIR="$5"

"$CLANG" -O1 -fno-discard-value-names -fno-builtin \
  -fno-optimize-sibling-calls \
  -I "$SRC_DIR" \
  -S -emit-llvm -o "$NAME.ll" \
  "$SRC_DIR/tests/$NAME.c"

"$OPT" -load-pass-plugin="$PLUGIN" \
  -passes=llvm-ir-to-lc3-pass \
  -disable-output -S "$NAME.ll"
