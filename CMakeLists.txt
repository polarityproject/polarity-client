cmake_minimum_required(VERSION 3.20)
project(SfvRollback VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT everywhere: the mod must not depend on a VC++ redistributable, and
# GekkoNet's own CMake forces /MT for its static build.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Street Fighter V is x64 only; configure with a 64-bit toolchain.")
endif()

option(SFV_BUILD_MOD "Build the in-game DLL and the version.dll proxy (Windows only)" ON)

include(FetchContent)

# Pinned dependencies. Bump deliberately: GekkoNet's wire protocol changes
# between versions, so both peers must run the same mod build.
FetchContent_Declare(gekkonet
  GIT_REPOSITORY https://github.com/HeatXD/GekkoNet.git
  GIT_TAG 9da2363d7c5a68cc77f8dfe2da7cf51d3afe3bee # 2026-10-25
  # The mod's change: gekko_confirmed_frame, so saves that can never be
  # loaded skip their copy (src/core/netplay.cpp). The wire protocol is unchanged.
  PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/third_party/gekkonet.patch
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake
  SOURCE_SUBDIR GekkoLib)
set(NO_ASIO_BUILD ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(gekkonet)

if(SFV_BUILD_MOD AND WIN32)
  FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG 8af6b4acae5a9388fd742b56fa79ece89d96f823)
  FetchContent_MakeAvailable(minhook)

  # The in-game overlay (src/mod/overlay.cpp): Dear ImGui with its Win32 and
  # Direct3D 11 backends. ImGui has no CMake build of its own.
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)
  # The overlay feeds the pads itself (from the mod's XInput hook, before the
  # game's copy is made neutral); the backend's own polling would read neutral
  # pads through that same hook.
  target_compile_definitions(imgui PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

  # Weyvelength: rooms on our server, peer-to-peer links through ICE
  #. Only its client is built into the mod;
  # its own CMake would configure the server and the shared client too, so
  # the targets are defined here. libjuice predates CMP0091: without the
  # default it would ignore the static CRT setting above.
  FetchContent_Declare(weyvelength
    GIT_REPOSITORY https://github.com/HeatXD/Weyvelength.git
    GIT_TAG d4e25c7da7aab18ff82d60d275ea95f794bb2ec9 # 2026-08-22
    # The mod's changes to the client: peer-to-peer links survive losing the
    # server (a match goes on), and a link says whether it is direct or
    # relayed through TURN (plus a relay-only switch for testing).
    PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/third_party/weyvelength.patch
                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake
    SOURCE_SUBDIR none)
  FetchContent_MakeAvailable(weyvelength)
  # A link that is up outlives its member leaving the room (the other player
  # lost the server; the match goes on). Applied here, on every configure,
  # rather than in PATCH_COMMAND, which ran once in builds fetched before it.
  execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/third_party/weyvelength-departed-links.patch
                          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake
                  WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE departed_links)
  if(NOT departed_links EQUAL 0)
    message(FATAL_ERROR "cannot apply third_party/weyvelength-departed-links.patch")
  endif()
  # weyve_set_port_range: the keeper's links on a fixed UDP range its
  # server's firewall opens (the mod does not call it).
  execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/third_party/weyvelength-port-range.patch
                          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake
                  WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE port_range)
  if(NOT port_range EQUAL 0)
    message(FATAL_ERROR "cannot apply third_party/weyvelength-port-range.patch")
  endif()
  # weyve_set_link_mux: the keeper's links on one UDP socket (the mod does not
  # call it); a link sends its description only once its socket is bound.
  execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/third_party/weyvelength-link-mux.patch
                          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake
                  WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE link_mux)
  if(NOT link_mux EQUAL 0)
    message(FATAL_ERROR "cannot apply third_party/weyvelength-link-mux.patch")
  endif()
  # A cap on what decoding one message may allocate, on both ends (a few
  # bytes claiming gigabytes ran the rooms server out of memory). The same
  # patch goes into sfvr-server's server image and native install.
  execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/third_party/weyvelength-decode-limit.patch
                          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake
                  WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE decode_limit)
  if(NOT decode_limit EQUAL 0)
    message(FATAL_ERROR "cannot apply third_party/weyvelength-decode-limit.patch")
  endif()
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
  set(NO_SERVER ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  add_subdirectory(${weyvelength_SOURCE_DIR}/Client/thirdparty/libjuice ${CMAKE_BINARY_DIR}/libjuice EXCLUDE_FROM_ALL)
  set(WEYVE_SHARED_INCLUDES ${weyvelength_SOURCE_DIR}/Shared ${weyvelength_SOURCE_DIR}/Shared/thirdparty/asio/include)
  add_library(weyvelength_client STATIC
    ${weyvelength_SOURCE_DIR}/Client/src/client.cpp
    ${weyvelength_SOURCE_DIR}/Client/src/p2p_mesh.cpp
    ${weyvelength_SOURCE_DIR}/Client/src/weyvelength.cpp)
  target_include_directories(weyvelength_client
    PUBLIC ${weyvelength_SOURCE_DIR}/Client/include
    PRIVATE ${weyvelength_SOURCE_DIR}/Client/include/private ${WEYVE_SHARED_INCLUDES})
  target_link_libraries(weyvelength_client PRIVATE juice-static ws2_32 mswsock)
  target_compile_options(weyvelength_client PRIVATE /utf-8)

  # The server, for testing on this PC (scripts/weyve-server.ps1).
  add_executable(weyve_server
    ${weyvelength_SOURCE_DIR}/Server/src/weyvelength_server.cpp
    ${weyvelength_SOURCE_DIR}/Server/src/main.cpp)
  target_include_directories(weyve_server PRIVATE
    ${weyvelength_SOURCE_DIR}/Server/include ${weyvelength_SOURCE_DIR}/Shared/thirdparty/spdlog ${WEYVE_SHARED_INCLUDES})
  target_link_libraries(weyve_server PRIVATE ws2_32 mswsock)
  target_compile_options(weyve_server PRIVATE /utf-8 /bigobj)

  add_executable(weyve_probe src/tools/weyve_probe.cpp)
  target_link_libraries(weyve_probe PRIVATE weyvelength_client)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Core: game-agnostic netplay driver, replay format, recorder and verifier.
add_library(sfvcore STATIC
  src/core/state.cpp
  src/core/compress.cpp
  src/core/json.cpp
  src/core/update_info.cpp
  src/core/texture_package.cpp
  src/core/look.cpp
  src/core/rules.cpp
  src/core/match_ticket.cpp
  src/core/keeper_protocol.cpp
  src/core/country_flags.cpp
  src/keeper/sign.cpp
  src/core/replay_file.cpp
  src/core/file_open.cpp
  src/core/recorder.cpp
  src/core/verifier.cpp
  src/core/replay_diff.cpp
  src/core/netplay.cpp
  src/core/gekko_bridge.cpp
  src/core/lobby.cpp
  src/core/match_code.cpp
  src/core/mux.cpp
  src/core/spectate.cpp
  src/core/spectate_relay.cpp
  src/core/png.cpp)
target_include_directories(sfvcore PUBLIC src)
target_link_libraries(sfvcore PUBLIC GekkoNet PRIVATE bcrypt) # bcrypt: update signatures (the codec, compress.cpp, is looked up at run time)

# Deterministic toy fighter and in-memory network used to exercise the core
# without the game.
add_library(sfvtestbed STATIC
  src/testbed/mock_game.cpp
  src/testbed/loopback.cpp
  src/testbed/match_runner.cpp)
target_link_libraries(sfvtestbed PUBLIC sfvcore)

add_executable(sfvr src/tools/sfvr.cpp)
target_link_libraries(sfvr PRIVATE sfvcore sfvtestbed)

enable_testing()
add_executable(sfvtests tests/tests.cpp)
target_link_libraries(sfvtests PRIVATE sfvcore sfvtestbed bcrypt)
add_test(NAME sfvtests COMMAND sfvtests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(SFV_BUILD_MOD AND WIN32)
  # sfvr-keeper (src/keeper/keeper.h): hosts the lobby channel and the custom
  # rooms. Built here for local tests; the server builds it on Linux
  # (scripts/keeper-bundle.ps1 copies its sources into sfvr-server).
  add_executable(sfvr_keeper src/keeper/main.cpp src/keeper/keeper.cpp)
  set_target_properties(sfvr_keeper PROPERTIES OUTPUT_NAME sfvr-keeper)
  target_link_libraries(sfvr_keeper PRIVATE sfvcore weyvelength_client bcrypt ws2_32)
  # sfvr-bot: a headless rooms player for tests (src/tools/sfvr_bot.cpp).
  add_executable(sfvr_bot src/tools/sfvr_bot.cpp)
  set_target_properties(sfvr_bot PROPERTIES OUTPUT_NAME sfvr-bot)
  target_link_libraries(sfvr_bot PRIVATE sfvcore weyvelength_client bcrypt ws2_32)
endif()

if(SFV_BUILD_MOD AND WIN32)
  add_library(sfv_rollback SHARED
    src/mod/dllmain.cpp
    src/mod/log.cpp
    src/mod/memscan.cpp
    src/mod/battle_driver.cpp
    src/mod/battle_setup.cpp
    src/mod/boot.cpp
    src/mod/files.cpp
    src/mod/updater.cpp
    src/mod/pak_textures.cpp
    src/mod/profile.cpp
    src/mod/report_upload.cpp
    src/mod/crash_report.cpp
    src/mod/flow.cpp
    src/mod/hitch.cpp
    src/mod/pacing.cpp
    src/mod/outside_probe.cpp
    src/mod/overlay.cpp
    src/mod/select_screen.cpp
    src/mod/textures.cpp
    src/mod/ue4.cpp
    src/mod/config.cpp
    src/mod/control.cpp
    src/mod/image.cpp
    src/mod/probes.cpp
    src/mod/sfv_game.cpp
    src/mod/status.cpp
    src/mod/replay_driver.cpp
    src/mod/stress_driver.cpp
    src/mod/synctest.cpp
    src/mod/tracer.cpp
    src/mod/watch.cpp
    src/mod/entitlements.cpp
    src/mod/mod_main.cpp
    src/mod/netplay_driver.cpp
    src/mod/udp_transport.cpp
    src/mod/rooms_client.cpp
    src/mod/ui_kit.cpp
    src/mod/hub_screen.cpp
    src/mod/rooms_screen.cpp
    src/mod/room_screen.cpp
    src/mod/watch_panel.cpp
    src/mod/fight_ui.cpp
    src/mod/settings_screen.cpp
    src/mod/weyve_service.cpp
    src/mod/fonts.rc)
  target_link_libraries(sfv_rollback PRIVATE sfvcore minhook imgui weyvelength_client ws2_32 iphlpapi shell32 winhttp dbghelp)

  # The bundled CJK font for names (third_party/fonts/README.md): too large
  # for the repository or the DLL, so downloaded once by its tag and checked;
  # releases ship it as SFVR/fonts/NotoSansCJK-Medium.ttc (package.ps1).
  set(SFV_CJK_FONT ${CMAKE_BINARY_DIR}/fonts/NotoSansCJK-Medium.ttc)
  file(DOWNLOAD https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/OTC/NotoSansCJK-Medium.ttc ${SFV_CJK_FONT}
       EXPECTED_HASH SHA256=197d5e1e019faca33a4d55931c7d68b8056f3b97cb862049f5cb8de9efdfb8ce)
  set_property(SOURCE src/mod/fonts.rc APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_SOURCE_DIR}/third_party/fonts/Barlow-Regular.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/Barlow-SemiBold.ttf
    ${CMAKE_SOURCE_DIR}/third_party/fonts/Barlow-Bold.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/Barlow-BoldItalic.ttf
    ${CMAKE_SOURCE_DIR}/third_party/fonts/ArchivoBlack-Regular.ttf)

  # The noise mask goes into the DLL (every copy of a build masks the same
  # words), as bytes: MSVC caps string literals at 16 KB.
  set(SFV_NOISE_FILE ${CMAKE_SOURCE_DIR}/data/sfv_noise.txt)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SFV_NOISE_FILE})
  file(READ ${SFV_NOISE_FILE} SFV_NOISE_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SFV_NOISE_BYTES "${SFV_NOISE_HEX}")
  file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/builtin_noise.h
       CONTENT "#pragma once\n// Generated from data/sfv_noise.txt by CMakeLists.txt.\nnamespace sfv::mod {\ninline constexpr unsigned char kBuiltinNoise[] = {${SFV_NOISE_BYTES}};\n}\n")
  # The Steam app -> entitlement code table (data/steam_grants.txt), the same way.
  set(SFV_GRANTS_FILE ${CMAKE_SOURCE_DIR}/data/steam_grants.txt)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SFV_GRANTS_FILE})
  file(READ ${SFV_GRANTS_FILE} SFV_GRANTS_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SFV_GRANTS_BYTES "${SFV_GRANTS_HEX}")
  file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/builtin_steam_grants.h
       CONTENT "#pragma once\n// Generated from data/steam_grants.txt by CMakeLists.txt.\nnamespace sfv::mod {\ninline constexpr unsigned char kSteamGrants[] = {${SFV_GRANTS_BYTES}};\n}\n")
  target_include_directories(sfv_rollback PRIVATE ${CMAKE_BINARY_DIR}/generated)
  if(MSVC)
    # Symbols for crash stacks (scripts/symbolize.py); the code stays optimised.
    target_compile_options(sfv_rollback PRIVATE /Zi)
    target_link_options(sfv_rollback PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  endif()

  # The loader. See src/proxy/xinput_proxy.cpp.
  add_library(xinput_proxy SHARED src/proxy/xinput_proxy.cpp)
  target_include_directories(xinput_proxy PRIVATE src)
  set_target_properties(xinput_proxy PROPERTIES OUTPUT_NAME "xinput1_3")

  add_library(version_proxy SHARED src/proxy/version_proxy.cpp)
  target_include_directories(version_proxy PRIVATE src)
  set_target_properties(version_proxy PROPERTIES OUTPUT_NAME "version")
endif()
