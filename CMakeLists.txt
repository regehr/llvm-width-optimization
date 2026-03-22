cmake_minimum_required(VERSION 3.20)
project(llvm-width-optimization)

set(WO_LLVM_INSTALL_DIR "" CACHE PATH "LLVM installation directory")

set(WO_LLVM_INCLUDE_DIR "${WO_LLVM_INSTALL_DIR}/include/llvm")
if(NOT EXISTS "${WO_LLVM_INCLUDE_DIR}")
  message(FATAL_ERROR
    "WO_LLVM_INSTALL_DIR (${WO_LLVM_INCLUDE_DIR}) is invalid.")
endif()

set(WO_VALID_INSTALLATION FALSE)
if(EXISTS "${WO_LLVM_INSTALL_DIR}/lib/cmake/llvm/LLVMConfig.cmake")
  set(WO_VALID_INSTALLATION TRUE)
endif()
if(EXISTS "${WO_LLVM_INSTALL_DIR}/lib64/cmake/llvm/LLVMConfig.cmake")
  set(WO_VALID_INSTALLATION TRUE)
endif()

if(NOT ${WO_VALID_INSTALLATION})
  message(FATAL_ERROR
    "LLVM installation directory (${WO_LLVM_INSTALL_DIR}) is invalid. "
    "Couldn't find LLVMConfig.cmake.")
endif()

list(APPEND CMAKE_PREFIX_PATH
  "${WO_LLVM_INSTALL_DIR}/lib/cmake/llvm/"
  "${WO_LLVM_INSTALL_DIR}/lib64/cmake/llvm/")

find_package(LLVM REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${WO_LLVM_INSTALL_DIR}")

find_program(
  WO_LLVM_OPT
  NAMES opt
  HINTS
    "${WO_LLVM_INSTALL_DIR}/bin"
    "${LLVM_TOOLS_BINARY_DIR}"
    "${LLVM_BINARY_DIR}/bin"
  REQUIRED
)

find_program(
  WO_LLVM_LIT
  NAMES llvm-lit lit
  HINTS
    "${WO_LLVM_INSTALL_DIR}/bin"
    "${LLVM_TOOLS_BINARY_DIR}"
    "${LLVM_BINARY_DIR}/bin"
  REQUIRED
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug)" FORCE)
endif()

add_compile_options(-Wall -fdiagnostics-color=always)

if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fvisibility-inlines-hidden"
  SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
if(SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
  add_compile_options(-fvisibility-inlines-hidden)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

add_subdirectory(lib)
add_subdirectory(test)

add_custom_target(
  check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/test/scripts/run_lit.py"
    --llvm-lit "${WO_LLVM_LIT}"
    "${PROJECT_BINARY_DIR}/test"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/test/scripts/run_width_opt_smoke.py"
    --repo-root "${PROJECT_SOURCE_DIR}"
    --opt-bin "${WO_LLVM_OPT}"
    --plugin "$<TARGET_FILE:WidthOpt>"
    --dir test
  DEPENDS WidthOpt
  VERBATIM
)
