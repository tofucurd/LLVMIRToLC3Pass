# LLVM-IR To LC-3 Assembly Pass

An out-of-tree LLVM pass that translates a subset of LLVM IR (generated from C) into LC-3 assembly.

## Supported Instructions

`add`, `sub`, `mul`, `and`, `or`, `shl`, `lshr`, `ashr`, `udiv`, `urem`, `alloca`, `load`, `store`, `br`, `icmp` (signed + unsigned), `phi`, `select`, `call`, `switch`, `getelementptr`, `trunc`, bitcast, and `llvm.smin/smax/umin/umax` intrinsics.

## Runtime Support

| Function | Description |
|---|---|
| `printf(fmt, ...)` | Formatted output — `%d`, `%s`, `%c` |
| `sprintf(buf, fmt, ...)` | Formatted output to buffer |
| `scanf(fmt, ...)` | Formatted input — `%d`, `%s`, `%c` |
| `sscanf(buf, fmt, ...)` | Formatted input from buffer |
| `printStr` / `printChar` / etc. | Legacy helpers (kept for compatibility) |

Pointer operators `&` and `*` are fully supported. Global variables, global arrays, and stack-allocated arrays work correctly.

## Pass Options

| Option | Default | Description |
|---|---|---|
| `-lc3-entry=<name>` | `LC3_main` | Entry function name |
| `-lc3-start-addr=<addr>` | `x3000` | Program start address |
| `-lc3-stack-base=<addr>` | `xFE00` | Stack base address |
| `-lc3-global-addr=<addr>` | _(end of program)_ | Global data section address |
| `-signed-mul` | off | Enable signed multiplication |
| `-no-comment` | off | Suppress generated comments |

## Build

```sh
# Requires: CMake >= 3.13, LLVM >= 12 (headers + libraries)
mkdir build && cd build
cmake ..
make
```

## Usage

```sh
# Compile C to LLVM IR
clang example.c -O1 -fno-discard-value-names -S -emit-llvm -o example.ll

# Run the pass
opt -load-pass-plugin=build/LLVMIRToLC3Pass.so \
    -passes="llvm-ir-to-lc3-pass" \
    -disable-output -S example.ll

# Assemble and simulate (requires lc3tools)
lc3as example.asm
lc3sim example.obj
```

## LC3.h

Include `LC3.h` in your C code for access to `printf`/`scanf` and memory helpers.  Do not include libc headers.  Use `int` or `unsigned int` instead of `char`.  The entry function must be named `LC3_main` (or the name passed to `-lc3-entry`).

## Register Convention

| Register | Role |
|---|---|
| R0 | I/O temporary (OUT / GETC operand) |
| R1–R3 | Computation temporaries |
| R4 | Global data base pointer |
| R5 | Frame pointer |
| R6 | Stack pointer |
| R7 | Return address / PHI predecessor label |

## Testing

```sh
# Full test suite (35 tests, 3 tiers)
cmake -B build && make -C build && ctest --test-dir build --output-on-failure

# Tiers:
#   LL tests (12) — verify pass generates output
#   C tests  (7)  — full .c → clang → opt pipeline
#   Sim tests (16) — full pipeline through lc3as + lc3sim with output verification

# Requires: lc3as and lc3sim on PATH for sim tests
```

## Limitations

- No floating-point support (LC-3 has no FPU)
- No signed division / modulo (use `unsigned` for `/` and `%`)
- Max 32 local variables per function (LC-3 LDR/STR immediate range)
- `fastcc` / tail-call functions need `-fno-optimize-sibling-calls`
- Format strings for `printf`/`scanf` must be compile-time constants
