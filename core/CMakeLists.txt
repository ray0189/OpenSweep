cmake_minimum_required(VERSION 3.20)

# Static MSVC runtime so OpenSweepCore.exe has no VC++ redistributable dependency.
# Set before project() so it applies to every target (policy CMP0091 is NEW with 3.20).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(OpenSweepCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(WIN32)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# ---------------------------------------------------------------------------
# Common warning / hardening flags
function(opensweep_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus /guard:cf)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "EXECUTABLE")
      target_link_options(${target} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Portable library (no OS-specific code; built and tested on every platform)
add_library(opensweep_portable STATIC
  src/portable/json.cpp
  src/portable/framing.cpp
  src/portable/protocol.cpp
  src/portable/stats.cpp
  src/portable/commands.cpp
  src/portable/telemetry.cpp
)
target_include_directories(opensweep_portable PUBLIC src/portable)
opensweep_set_warnings(opensweep_portable)

# ---------------------------------------------------------------------------
# Unit tests (portable)
enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_json.cpp
  tests/test_framing.cpp
  tests/test_protocol.cpp
  tests/test_ringbuffer.cpp
  tests/test_stats.cpp
  tests/test_commands.cpp
)
target_link_libraries(core_tests PRIVATE opensweep_portable)
opensweep_set_warnings(core_tests)
add_test(NAME core_tests COMMAND core_tests)

# ---------------------------------------------------------------------------
# Windows service executable
if(WIN32)
  add_executable(OpenSweepCore
    src/win/main.cpp
    src/win/app.cpp
    src/win/service.cpp
    src/win/pipe_server.cpp
    src/win/metrics.cpp
    src/win/topology.cpp
    src/win/log.cpp
  )
  target_include_directories(OpenSweepCore PRIVATE src/win)
  target_link_libraries(OpenSweepCore PRIVATE opensweep_portable advapi32 kernel32 psapi shell32 ole32 uuid)
  opensweep_set_warnings(OpenSweepCore)
  if(MSVC)
    # Console subsystem: --console/--install/--version print to the console; the SCM ignores it.
    target_link_options(OpenSweepCore PRIVATE /SUBSYSTEM:CONSOLE)
  endif()
endif()
