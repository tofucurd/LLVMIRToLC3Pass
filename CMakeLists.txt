cmake_minimum_required(VERSION 3.13)
project(LLVMIRToLC3Pass)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Enforce C++17 (Required for modern LLVM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2. Find LLVM
find_package(LLVM REQUIRED CONFIG)

# 3. Add LLVM definitions
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# 4. Create the library
add_library(LLVMIRToLC3Pass MODULE LLVMIRToLC3Pass.cpp)

# 5. Do not prefix with 'lib'
set_target_properties(LLVMIRToLC3Pass PROPERTIES PREFIX "")

# 6. [CRITICAL FIX] Handle macOS Linker Flags
if(APPLE)
    # Allows undefined symbols that will be resolved by the host executable (opt)
    target_link_options(LLVMIRToLC3Pass PRIVATE "-Wl,-undefined,dynamic_lookup")
endif()

# 7. Handle RTTI
if(NOT LLVM_ENABLE_RTTI)
    set_target_properties(LLVMIRToLC3Pass PROPERTIES COMPILE_FLAGS "-fno-rtti")
endif()

# 8. CTest suite
enable_testing()

find_program(OPT_EXECUTABLE opt REQUIRED)
find_program(CLANG_EXECUTABLE clang REQUIRED)

# add_lc3_test(name) — positive test: pass must emit "One file generated"
macro(add_lc3_test name)
  add_test(NAME ${name}
    COMMAND ${OPT_EXECUTABLE}
      -load-pass-plugin=$<TARGET_FILE:LLVMIRToLC3Pass>
      -passes=llvm-ir-to-lc3-pass
      -disable-output -S
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ll
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "One file generated")
endmacro()

# add_lc3_c_test(name) — compile tests/<name>.c to .ll with clang, then run pass
macro(add_lc3_c_test name)
  add_test(NAME ${name}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_c_test.sh
      ${name}
      ${CLANG_EXECUTABLE}
      ${OPT_EXECUTABLE}
      $<TARGET_FILE:LLVMIRToLC3Pass>
      ${CMAKE_CURRENT_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "One file generated")
endmacro()

# add_lc3_fail_test(name) — negative test: pass must abort with error
macro(add_lc3_fail_test name)
  add_test(NAME ${name}
    COMMAND ${OPT_EXECUTABLE}
      -load-pass-plugin=$<TARGET_FILE:LLVMIRToLC3Pass>
      -passes=llvm-ir-to-lc3-pass
      -disable-output -S
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ll
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "No File Generated")
endmacro()

add_lc3_test(arith)
add_lc3_test(shifts)
add_lc3_test(divrem)
add_lc3_test(control_flow)
add_lc3_test(calls)
add_lc3_test(globals)
add_lc3_test(stack_arrays)
add_lc3_test(pointers)
add_lc3_test(printf)
add_lc3_test(scanf)
add_lc3_test(sprintf)
add_lc3_test(strings_sprintf)
add_lc3_test(strings_sscanf)
add_lc3_test(strings_ptr)
add_lc3_fail_test(unsupported)

# C-source tests (full pipeline: .c → clang → .ll → opt → .asm)
add_lc3_c_test(c_arith)
add_lc3_c_test(c_globals)
add_lc3_c_test(c_arrays)
add_lc3_c_test(c_pointers)
add_lc3_c_test(c_printf)
add_lc3_c_test(c_control)
add_lc3_c_test(c_qsort)

# Simulation tests (full pipeline: .c→.ll→opt→.asm→lc3as→lc3sim)
find_program(LC3AS_EXECUTABLE lc3as REQUIRED)
find_program(LC3SIM_EXECUTABLE lc3sim REQUIRED)

# add_lc3_sim_test(name [EXPECT pattern]) — assemble + simulate a .ll test
macro(add_lc3_sim_test name)
  add_test(NAME sim_${name}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_sim_test.sh
      ${name}
      ${OPT_EXECUTABLE}
      $<TARGET_FILE:LLVMIRToLC3Pass>
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ll
      "${ARGN}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endmacro()

add_lc3_sim_test(arith "20 ")
add_lc3_sim_test(shifts "31 ")
add_lc3_sim_test(divrem "32 ")
add_lc3_sim_test(control_flow "10 ")
add_lc3_sim_test(calls "120 ")
add_lc3_sim_test(globals "63 ")
add_lc3_sim_test(stack_arrays "43 ")
add_lc3_sim_test(pointers "99 ")
add_lc3_sim_test(printf "x=-42 s=hello c=A")
add_lc3_sim_test(strings_ptr "hellohello")

# C simulation tests (C → .ll → opt → lc3as → lc3sim)
macro(add_lc3_c_sim_test name)
  add_test(NAME sim_${name}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_c_sim_test.sh
      ${name}
      ${CLANG_EXECUTABLE}
      ${OPT_EXECUTABLE}
      $<TARGET_FILE:LLVMIRToLC3Pass>
      ${CMAKE_CURRENT_SOURCE_DIR}
      "${ARGN}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endmacro()

add_lc3_c_sim_test(c_qsort "0 10 20 30 40 50 60 70")
add_lc3_c_sim_test(c_arith "964")
add_lc3_c_sim_test(c_globals "89")
add_lc3_c_sim_test(c_arrays "52")
add_lc3_c_sim_test(c_pointers "99")
add_lc3_c_sim_test(c_printf "x=42")
add_lc3_c_sim_test(c_control "120")