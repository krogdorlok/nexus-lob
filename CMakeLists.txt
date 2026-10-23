cmake_minimum_required(VERSION 3.20)
project(nexus_lob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NEXUS_NATIVE "Tune for the build machine (-march=native)" OFF)
option(NEXUS_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NEXUS_TSAN "Build with ThreadSanitizer" OFF)
option(NEXUS_WERROR "Treat warnings as errors (CI debug job only)" OFF)

if(NEXUS_ASAN AND NEXUS_TSAN)
  message(FATAL_ERROR "NEXUS_ASAN and NEXUS_TSAN are mutually exclusive")
endif()

# Sanitizers and -march apply to every target, dependencies included, so instrumentation and
# codegen stay consistent across the whole binary.
if(NEXUS_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(NEXUS_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if(NEXUS_NATIVE)
  add_compile_options(-march=native)
endif()

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest benchmark)

# Warnings for our own targets only.
add_library(nexus_warnings INTERFACE)
target_compile_options(nexus_warnings INTERFACE -Wall -Wextra -Wpedantic
                       $<$<BOOL:${NEXUS_WERROR}>:-Werror>)

add_library(nexus INTERFACE)
target_include_directories(nexus INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nexus INTERFACE nexus_warnings)

enable_testing()
foreach(t test_pool test_order_map test_ladder test_price_level test_matching test_no_alloc)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE nexus GTest::gtest_main)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
if(NEXUS_ASAN OR NEXUS_TSAN)
  target_compile_definitions(test_no_alloc PRIVATE NEXUS_SANITIZED)
endif()

# Default size keeps sanitizer runs quick; the full run is
# fuzz_differential --seeds 200 --commands 100000 (Release).
add_executable(fuzz_differential tests/fuzz_differential.cpp)
target_link_libraries(fuzz_differential PRIVATE nexus)
add_test(NAME fuzz_differential COMMAND fuzz_differential --seeds 20 --commands 20000)
