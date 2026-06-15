#!/bin/sh
# run_sim_test.sh <name> <opt> <plugin> <llfile> [expect]
set -e
NAME="$1" OPT="$2" PLUGIN="$3" LLFILE="$4" EXPECT="${5:-}"

ASM="${NAME}.asm"
OBJ="${NAME}.obj"

# Clean previous artifacts for this test
rm -f "$ASM" "$OBJ"

# 1. Generate .asm — use the source stem explicitly.
# We copy the .ll to a temp file with the right source name so the
# pass derives the correct output filename.
SRC_STEM=$(basename "$LLFILE" .ll)
TMP_LL="_sim_${NAME}.ll"
cp "$LLFILE" "$TMP_LL"
ERRS=$("$OPT" -load-pass-plugin="$PLUGIN" -passes=llvm-ir-to-lc3-pass \
       -disable-output -S "$TMP_LL" 2>&1) || true
rm -f "$TMP_LL"

if ! echo "$ERRS" | grep -q "One file generated"; then
  echo "FAIL: pass did not generate file"
  echo "$ERRS"
  exit 1
fi

# Find the actual .asm (named after the temp .ll stem)
ACTUAL_ASM="_sim_${NAME}.asm"
if [ ! -f "$ACTUAL_ASM" ]; then
  echo "FAIL: $ACTUAL_ASM not found"
  ls *.asm 2>/dev/null
  exit 1
fi
mv "$ACTUAL_ASM" "$ASM"

# 2. Assemble
ASM_ERRS=$(lc3as "$ASM" 2>&1) || true
if ! echo "$ASM_ERRS" | grep -q "0 errors"; then
  echo "FAIL: lc3as errors"
  echo "$ASM_ERRS"
  exit 1
fi

# 3. Simulate
SIM_OUT=$(echo "continue" | lc3sim "$OBJ" 2>&1) || true

# 4. Must reach HALT
if ! echo "$SIM_OUT" | grep -q "TRAP_HALT"; then
  echo "FAIL: did not reach HALT"
  echo "$SIM_OUT" | tail -10
  exit 1
fi

# 5. Optional expected output
if [ -n "$EXPECT" ]; then
  if ! echo "$SIM_OUT" | grep -qF "$EXPECT"; then
    echo "FAIL: expected '$EXPECT' not in output"
    echo "$SIM_OUT" | grep -v "^PC=\|^R[0-7]=\|^---\|^  (lc3sim)\|^ \|^Welcome\|^The\|^tools\|^Public\|^conditions\|^absolutely\|^Have\|^of it\|^Loaded\|^distributed\|^the LC-3\|^ " | head -10
    exit 1
  fi
fi

echo "PASS: $NAME halted correctly"
rm -f "$ASM" "$OBJ"
exit 0
