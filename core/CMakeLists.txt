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
  src/portable/trace.cpp
  src/portable/baseline.cpp
  src/portable/incident.cpp
  src/portable/guard.cpp
  src/portable/scheduler_policy.cpp
  src/portable/memory_policy.cpp
  src/portable/journal.cpp
  src/portable/control.cpp
  src/portable/heal.cpp
  src/portable/engine.cpp
  src/portable/commands2.cpp
  src/portable/kernel_stats.cpp
  src/portable/kernel_guard.cpp
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
  tests/test_trace.cpp
  tests/test_baseline.cpp
  tests/test_incident.cpp
  tests/test_guard.cpp
  tests/test_scheduler_policy.cpp
  tests/test_memory_policy.cpp
  tests/test_journal.cpp
  tests/test_control.cpp
  tests/test_heal.cpp
  tests/test_engine.cpp
  tests/test_commands2.cpp
  tests/test_kernel_stats.cpp
  tests/test_kernel_guard.cpp
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
    src/win/collectors.cpp
    src/win/events.cpp
    src/win/actuator.cpp
    src/win/process_info.cpp
    src/win/storage.cpp
    src/win/heal_executor.cpp
    src/win/etw.cpp
  )
  target_link_libraries(OpenSweepCore PRIVATE pdh wevtapi)  # collectors (PDH) + events (wevtapi)
  target_include_directories(OpenSweepCore PRIVATE src/win)
  target_link_libraries(OpenSweepCore PRIVATE opensweep_portable advapi32 kernel32 psapi shell32 ole32 uuid)
  opensweep_set_warnings(OpenSweepCore)
  if(MSVC)
    # Console subsystem: --console/--install/--version print to the console; the SCM ignores it.
    target_link_options(OpenSweepCore PRIVATE /SUBSYSTEM:CONSOLE)
  endif()
endif()

# ---------------------------------------------------------------------------
# Windows-only tests: collectors, event log, actuator (touch the real system; run in the
# `windows` job of .github/workflows/core.yml via ctest).
if(WIN32)
  add_executable(core_win_tests
    tests/test_main.cpp
    tests/win/test_win_collectors.cpp
    tests/win/test_win_etw.cpp
    src/win/etw.cpp
    src/win/collectors.cpp
    src/win/events.cpp
    src/win/actuator.cpp
    src/win/process_info.cpp
    src/win/log.cpp
  )
  target_include_directories(core_win_tests PRIVATE src/win tests)
  target_link_libraries(core_win_tests PRIVATE opensweep_portable pdh wevtapi psapi advapi32 kernel32 shell32 ole32 uuid)
  opensweep_set_warnings(core_win_tests)
  add_test(NAME core_win_tests COMMAND core_win_tests)
  set_tests_properties(core_win_tests PROPERTIES TIMEOUT 180)
endif()
