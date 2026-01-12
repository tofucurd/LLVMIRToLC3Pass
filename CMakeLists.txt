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