cmake_minimum_required(VERSION 3.20)
project(quarry
        VERSION 0.1.0
        DESCRIPTION "A columnar query engine: encoded storage with zone-map pruning, vectorized push-based execution"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(QUARRY_BUILD_TESTS "Build the test suite" ON)
option(QUARRY_BUILD_APPS  "Build the CLI tools and daemons" ON)
option(QUARRY_BUILD_BENCH "Build the benchmark harness" ON)
option(QUARRY_WERROR      "Treat warnings as errors" OFF)
option(QUARRY_NATIVE      "Build for the host microarchitecture" OFF)

set(QUARRY_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread, undefined, or empty")
set_property(CACHE QUARRY_SANITIZER PROPERTY STRINGS "" address thread undefined)

# ---------------------------------------------------------------------------
# Warning / sanitizer interface target
# ---------------------------------------------------------------------------
add_library(quarry_flags INTERFACE)
target_compile_options(quarry_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Wunused -Woverloaded-virtual -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough
)
if(QUARRY_WERROR)
  target_compile_options(quarry_flags INTERFACE -Werror)
endif()

if(QUARRY_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" QUARRY_HAS_MARCH_NATIVE)
  if(QUARRY_HAS_MARCH_NATIVE)
    target_compile_options(quarry_flags INTERFACE -march=native)
  else()
    # Apple clang on arm64 rejects -march=native; -mcpu=native is the equivalent.
    check_cxx_compiler_flag("-mcpu=native" QUARRY_HAS_MCPU_NATIVE)
    if(QUARRY_HAS_MCPU_NATIVE)
      target_compile_options(quarry_flags INTERFACE -mcpu=native)
    endif()
  endif()
endif()

if(QUARRY_SANITIZER)
  message(STATUS "quarry: enabling ${QUARRY_SANITIZER} sanitizer")
  target_compile_options(quarry_flags INTERFACE
    -fsanitize=${QUARRY_SANITIZER} -fno-omit-frame-pointer -g)
  target_link_options(quarry_flags INTERFACE -fsanitize=${QUARRY_SANITIZER})
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(QUARRY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(QUARRY_BUILD_APPS)
  add_subdirectory(apps)
endif()
if(QUARRY_BUILD_BENCH)
  add_subdirectory(bench)
endif()
