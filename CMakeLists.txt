cmake_minimum_required(VERSION 3.18.2)
cmake_policy(VERSION 3.18.2)
cmake_policy(SET CMP0135 NEW)

# build.zig is the master build of this project — it defines the targets, the options and the
# matrix that gets verified. This file exists for the one case zig cannot serve: the MSVC ABI
# on Windows, whose SDK zig does not ship. Everything here mirrors build.zig rather than
# leading it; when the two disagree, build.zig is right. See AGENTS.md.
#
#   cmake -B build -DENABLE_TESTS=ON -DENABLE_BENCHMARKS=ON
#   cmake --build build --config Release
#   ctest --test-dir build -C Release

set(PROJECT_NAME hostmem)
project(${PROJECT_NAME} C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Mirrors -Dtests, -Dbenchmarks and -Dsanitize of build.zig, defaulting the same way.
option(ENABLE_BENCHMARKS "Build the bench_* binaries" OFF)
option(ENABLE_TESTS "Build the googletest binaries" OFF)
option(ENABLE_SANITIZERS "Build with AddressSanitizer, plus UndefinedBehaviorSanitizer on non-MSVC toolchains (MSVC ships no UBSan)" OFF)
option(ENABLE_THREAD_SANITIZER "Build with ThreadSanitizer (mutually exclusive with ENABLE_SANITIZERS)" OFF)

# Set before any target is declared, so library, tests and benchmarks are instrumented the same
# way — mixing an instrumented library with an uninstrumented test binary produces false reports.
if(ENABLE_SANITIZERS AND ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_THREAD_SANITIZER cannot be combined; ThreadSanitizer does not run together with AddressSanitizer")
endif()

if(ENABLE_SANITIZERS)
  if(MSVC)
    # MSVC ships AddressSanitizer only, there is no UndefinedBehaviorSanitizer counterpart.
    # This is also the one sanitizer build.zig cannot offer: zig does not ship the asan runtime.
    message(STATUS "Sanitizers enabled: address")
    add_compile_options(/fsanitize=address /Zi)
  else()
    message(STATUS "Sanitizers enabled: address, undefined")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

if(ENABLE_THREAD_SANITIZER)
  if(MSVC)
    message(FATAL_ERROR "ENABLE_THREAD_SANITIZER is not supported by MSVC")
  endif()
  message(STATUS "Sanitizers enabled: thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

if(MSVC)
  # The sources are UTF-8 and their comments carry em dashes. Without this MSVC reads them in
  # the system codepage and warns (C4819) or mangles them; the flag is not optional here.
  add_compile_options(/utf-8)
endif()

FILE(GLOB HOSTMEM_SRC "src/*.c")
FILE(GLOB HOSTMEM_HEADERS "include/hostmem/*.h")

add_library(${PROJECT_NAME} STATIC ${HOSTMEM_SRC} ${HOSTMEM_HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

if(MSVC)
  source_group("src" FILES ${HOSTMEM_SRC})
  source_group("include" FILES ${HOSTMEM_HEADERS})
  # debug symbols for a parent project stepping into this one
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/JMC>)
endif()

install(TARGETS ${PROJECT_NAME} ARCHIVE DESTINATION lib)
install(FILES ${HOSTMEM_HEADERS} DESTINATION include/hostmem)

if(ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
