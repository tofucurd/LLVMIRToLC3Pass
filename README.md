# A Simple LLVM-IR To LC-3 Assembly LLVM Pass

## Introduction

This is a simple LLVM pass that can translate a subset of LLVM-IR Instructions (generated from C code) into LC-3 Assembly. It Support the following LLVM-IR Instructions: ``add``,``and``,``shl``,``mul``,``alloca``,``store``,``br``,``load``,``icmp``,``phi``,``select``,``call``. Note that there is no need to translate Integer Coversion Instructions (``sext``,``zext``,``trunc``,etc.), because in LC-3 everything has a fixed 16-bit length. Also, this pass cannot handle any floating point instructions, because LC-3 doesn't support them. This pass is simple enough that it doesn't have the concept of stack, heap, frame, etc. It treats every LLVM IR virtual register as an address in memory arbitrarily and does no optimization.

## Build

First, you need to clone this repo.

Second, install the dependents:

- CMake (version >= 3.13)
- LLVM toolchain, **headers and libraries** (LLVM version >= 12)

Then, build with CMake:
```
# in the repo directory
mkdir build && cd build
cmake ..
make
```
## Usage

After you get a ``LLVMIRToLC3Pass.so`` in ``build/``, you can use it to translate the LLVM-IR file. To do so, take the ``example.c`` in this repo for example.

First, compile the C source file into LLVM-IR file. Note that there must be no optimization enabled.

```
clang example.c -O0 -S -emit-llvm -o example.ll
```

Second, use ``opt`` to run the pass. Note that option ``-lc3-start-addr=<addr>`` specifies the starting address of the assembly file, the default value is ``x3000``, and option ``-signed-mul`` enables the signed integer mulplication support, otherwise the pass will only translate unsigned multiplication.

```
opt -load-pass-plugin=build/LLVMIRToLC3Pass.so -passes="llvm-ir-to-lc3-pass" -lc3-start-addr="x3000" -signed-mul -disable-output -S example.ll
```

Then, you will get ``eample.asm`` that can be recognized by ``lc3as``.

## Code With the Pass

This project also provides a ``LC3.h`` header for you to access the memory and to print something to screen when writing C code.

To begin with, first include the ``LC3.h`` header. Note that LC-3 cannot call functions, so you cannot use any libc functions.

Then, you must write only one function, i.e. a ``main()`` function with no arguments. Note that this pass cannot handle all instructions, so be carefull to not write any unsupported operations (e.g. right shift or division).

The pass provides 10 functions for special operations, you can check them in ``LC3.h``.

If you get error message begin with ``Unsupported instruction:``, then it means you must change your code to fit the pass.

## TODO

Add support for stack allocation.