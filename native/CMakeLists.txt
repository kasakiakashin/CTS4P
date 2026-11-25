cmake_minimum_required(VERSION 3.25)

# Static MSVC runtime + static vcpkg triplet => one self-contained winmm.dll.
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(cts4p_native VERSION 0.5.0 LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "CTS4P native must be built with MSVC for x64 (The Sims 4 is 64-bit).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(minhook CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
if(TARGET zstd::libzstd_static)
  set(CTS4P_ZSTD zstd::libzstd_static)
else()
  set(CTS4P_ZSTD zstd::libzstd)
endif()

if(TARGET LibDataChannel::LibDataChannelStatic)
  set(CTS4P_RTC LibDataChannel::LibDataChannelStatic)
else()
  set(CTS4P_RTC LibDataChannel::LibDataChannel)
endif()

# Strict warnings for our code only; dependency headers are "external" (/external:W0).
function(cts4p_strict target)
  set(flags /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP
            /external:anglebrackets /external:W0)
  target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${flags}>")
  target_compile_definitions(${target} PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_link_options(${target} PRIVATE /WX $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endfunction()

# ---------------------------------------------------------------- winmm.dll proxy
add_library(cts4p_winmm SHARED
  src/dllmain.cpp
  src/bridge/bridge.cpp
  src/build/build_observer.cpp
  src/hook/memory.cpp
  src/sync/compress.cpp
  src/sync/manifest.cpp
  src/sync/save_transfer.cpp
  src/sync/tray.cpp
  src/core/app.cpp
  src/core/config.cpp
  src/core/debug_pipe.cpp
  src/core/log.cpp
  src/core/paths.cpp
  src/diag/diagnostics.cpp
  src/hook/hooks.cpp
  src/ipc/ipc_api.cpp
  src/net/peer_link.cpp
  src/net/protocol.cpp
  src/net/room_code.cpp
  src/net/selftest.cpp
  src/net/session.cpp
  src/net/signaling.cpp
  src/render/dx11_hook.cpp
  src/ui/overlay.cpp
  src/ui/panels.cpp
  src/ui/theme.cpp
  src/ui/view_model.cpp
  src/version.rc)
set_target_properties(cts4p_winmm PROPERTIES OUTPUT_NAME winmm)
target_include_directories(cts4p_winmm PRIVATE src)
target_link_libraries(cts4p_winmm PRIVATE
  minhook::minhook imgui::imgui ${CTS4P_RTC} nlohmann_json::nlohmann_json ${CTS4P_ZSTD}
  d3d11 dxgi shell32 ole32 advapi32 user32 crypt32)
# Keep PDBs for crash analysis without shipping debug code.
target_compile_options(cts4p_winmm PRIVATE $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/Zi>)
target_link_options(cts4p_winmm PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
cts4p_strict(cts4p_winmm)

# ---------------------------------------------------------------- tests
add_executable(test_proxy tests/test_proxy.cpp)
cts4p_strict(test_proxy)

add_executable(test_net tests/test_net.cpp)
target_link_libraries(test_net PRIVATE nlohmann_json::nlohmann_json)
cts4p_strict(test_net)

add_executable(test_build tests/test_build.cpp)
cts4p_strict(test_build)

add_executable(test_tray tests/test_tray.cpp src/sync/tray.cpp src/core/paths.cpp)
target_include_directories(test_tray PRIVATE src)
cts4p_strict(test_tray)

add_executable(test_overlay tests/test_overlay.cpp)
target_link_libraries(test_overlay PRIVATE d3d11 dxgi windowscodecs ole32)
cts4p_strict(test_overlay)

# Tests load winmm.dll from their own folder (same output directory as the DLL).
foreach(t test_proxy test_net test_overlay test_build)
  add_dependencies(${t} cts4p_winmm)
endforeach()

enable_testing()
add_test(NAME proxy COMMAND test_proxy)
add_test(NAME net_loopback COMMAND test_net loopback)
add_test(NAME overlay COMMAND test_overlay)
add_test(NAME tray COMMAND test_tray)

# ---------------------------------------------------------------- tools
# Runs scripts on the game's own Python 3.7 (compile .pyc, test the ctypes bridge).
add_executable(py37host tools/py37host/py37host.c)
target_compile_options(py37host PRIVATE /W4 /WX)
target_compile_definitions(py37host PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
