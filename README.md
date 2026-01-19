# A Simple LLVM-IR To LC-3 Assembly LLVM Pass

## Introduction

This is a simple LLVM pass that can translate a subset of LLVM-IR Instructions (generated from C code) into LC-3 Assembly. It supports the following LLVM-IR Instructions: ``add``,``and``,``shl``,``mul``,``alloca``,``store``,``br``,``load``,``icmp``,``phi``,``select``,``call``,``udiv``,``urem``,``lshr``,``switch``.

Note that:
- This pass uses R6 as the stack pointer, R5 as the frame pointer and R7 as PC saver.
- This pass treats every unsigned number as signed.
- This pass cannot handle any floating point instructions, because LC-3 doesn't support them.
- It treats every LLVM IR virtual register as an address in memory arbitrarily.

## Build

First, you need to clone this repo.

Second, install the dependents:

- CMake (version >= 3.13)
- LLVM toolchain, **headers and libraries** (version >= 12)

Then, build with CMake:
```
# in the repo directory
mkdir build && cd build
cmake ..
make
```
## Usage

After you get a ``LLVMIRToLC3Pass.so`` in ``build/``, you can use it to translate the LLVM-IR file. To do so, take the ``example.c`` in this repo for example.

First, compile the C source file into LLVM-IR file.

```
# in the repo directory
clang example.c -O1 -S -emit-llvm -o example.ll
```

Second, use ``opt`` to run the pass.

Special Options:

- ``-lc3-start-addr=<addr>`` -  Specify the starting address of the LC-3 program, default ``"x3000"``
- ``-lc3-stack-base=<addr>`` - Specify the base address of the stack memory of the LC-3 program, default ``"xFE00"``
- ``-signed-mul`` - Enable signed multiplication, default off.

An example to run the pass with options:

```
# in the repo directory
opt -load-pass-plugin=build/LLVMIRToLC3Pass.so \
    -passes="llvm-ir-to-lc3-pass" \
    -lc3-start-addr="x4000" \
    -lc3-stack-base="x5000" \
    -signed-mul \
    -disable-output -S example.ll
```

If you get error message ``Unsupported instruction: <LLVM IR Inst>``, then it means you must change your code to fit the pass.

If you get error message ``Too many local variables: <Count>``, then it means the count of the local variable exceeded the max count LC-3 ISA support. You can compile the origin C code with a higher optimization level to try to solve this problem. Or, you can try to split a long function into several small functions.

If there is no error, you will get a ``.asm`` file that can be recognized by ``lc3as``.

## Code With the Pass

This project also provides a ``LC3.h`` header for you to access the memory and to print something to screen when writing C code.

To begin with, first include the ``LC3.h`` header. Note that you cannot include any libc headers.

The pass provides 10 functions for special operations, you can check them out in ``LC3.h``.

Do not use signed types as variable type (except main), avoid using ``char``. Try to represent anything in ``unsigned``.

Note that this pass cannot handle all instructions, so be carefull to not write any unsupported operations.

## TODO

Add support for global variable and array.