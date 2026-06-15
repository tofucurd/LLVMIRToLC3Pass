#!/bin/sh
# run_c_sim_test.sh <name> <clang> <opt> <plugin> <src_dir> [expect]
set -e
NAME="$1" CLANG="$2" OPT="$3" PLUGIN="$4" SRC_DIR="$5" EXPECT="${6:-}"

ASM="${NAME}.asm"  OBJ="${NAME}.obj"  LL="${NAME}.ll"
rm -f "$ASM" "$OBJ" "$LL"

# 1. C → .ll
"$CLANG" -O1 -fno-discard-value-names -fno-builtin \
   -fno-optimize-sibling-calls \
   -I "$SRC_DIR" -S -emit-llvm -o "$LL" \
   "$SRC_DIR/tests/${NAME}.c"

# 2. .ll → .asm
ERRS=$("$OPT" -load-pass-plugin="$PLUGIN" -passes=llvm-ir-to-lc3-pass \
       -disable-output -S "$LL" 2>&1) || true
if ! echo "$ERRS" | grep -q "One file generated"; then
  echo "FAIL: pass error" && echo "$ERRS" && exit 1
fi

# 3. Assemble
ASM_ERRS=$(lc3as "$ASM" 2>&1) || true
if ! echo "$ASM_ERRS" | grep -q "0 errors"; then
  echo "FAIL: lc3as errors" && echo "$ASM_ERRS" && exit 1
fi

# 4. Simulate
SIM_OUT=$(echo "continue" | lc3sim "$OBJ" 2>&1) || true
if ! echo "$SIM_OUT" | grep -q "TRAP_HALT"; then
  echo "FAIL: no HALT" && echo "$SIM_OUT" | tail -10 && exit 1
fi

# 5. Check expected output
if [ -n "$EXPECT" ]; then
  if ! echo "$SIM_OUT" | grep -qF "$EXPECT"; then
    echo "FAIL: expected '$EXPECT' not in output"
    echo "$SIM_OUT" | grep -v "^PC=\|^R[0-7]=\|^---\|^  (lc3sim)" | head -5
    exit 1
  fi
fi

echo "PASS: $NAME halted correctly"
rm -f "$ASM" "$OBJ" "$LL"
exit 0
