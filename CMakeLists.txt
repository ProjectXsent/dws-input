cmake_minimum_required(VERSION 3.21)

# --- Static CRT by default: the proxies sit in a game folder and should not need
# --- the VC++ redistributable. Must be set before project().
option(DWS_STATIC_CRT "Link the MSVC runtime statically (/MT)" ON)
if(DWS_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(DWSInput VERSION 1.0.0
        DESCRIPTION "DWS Input - SDL3 gamepad proxies for winmm, dinput and dinput8"
        LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "DWS Input builds with MSVC / clang-cl on Windows "
                        "(the winmm forwarders use MSVC linker pragmas).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# SDL3: use an installed copy if there is one (vcpkg, -DSDL3_DIR=...), otherwise
# download and build SDL3 as a shared library (SDL3.dll ships next to the proxies).
# ---------------------------------------------------------------------------
set(DWS_SDL3_TAG "release-3.2.10" CACHE STRING "SDL3 git tag used when SDL3 is fetched")
option(DWS_FETCH_SDL3 "Download and build SDL3 if it is not found" ON)

find_package(SDL3 CONFIG QUIET)
if(NOT TARGET SDL3::SDL3)
    if(NOT DWS_FETCH_SDL3)
        message(FATAL_ERROR "SDL3 not found. Install it (e.g. vcpkg install sdl3), pass -DSDL3_DIR=..., "
                            "or enable -DDWS_FETCH_SDL3=ON.")
    endif()
    message(STATUS "SDL3 not found, fetching ${DWS_SDL3_TAG}")
    include(FetchContent)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        ${DWS_SDL3_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------------------
# Shared core (header-only): INI reader with per-DLL override sections, log
# naming, and the control-source tables used by winmm + dinput.
# ---------------------------------------------------------------------------
add_library(dws_core INTERFACE)
target_include_directories(dws_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_definitions(dws_core INTERFACE _CRT_SECURE_NO_WARNINGS)
target_compile_options(dws_core INTERFACE /utf-8 /W3)
target_link_libraries(dws_core INTERFACE SDL3::SDL3)

# ---------------------------------------------------------------------------
# The three proxies. Output names are fixed: the game loads them by name.
# ---------------------------------------------------------------------------
function(dws_add_proxy target dllname)   # extra args = sources, then optional LINK <libs>
    cmake_parse_arguments(P "" "" "SOURCES;LINK" ${ARGN})
    add_library(${target} SHARED ${P_SOURCES})
    target_link_libraries(${target} PRIVATE dws_core ${P_LINK})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${dllname}
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        # keep the generated import libs away from the DLLs: "winmm.lib" next to
        # winmm.dll would be confusing (and the winmm proxy must never link winmm.lib)
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/implib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
endfunction()

dws_add_proxy(dws_winmm   winmm
    SOURCES src/winmm/winmm.cpp     src/winmm/winmm.def)

dws_add_proxy(dws_dinput  dinput
    SOURCES src/dinput/dinput.cpp   src/dinput/dinput.def)

# dinput8 uses the DirectInput8 interface IIDs / effect GUIDs; dxguid provides them.
# (Never link dinput8.lib: the proxy loads the real dinput8.dll itself.)
dws_add_proxy(dws_dinput8 dinput8
    SOURCES src/dinput8/dinput8.cpp src/dinput8/dinput8.def
    LINK dxguid)

# ---------------------------------------------------------------------------
# install(): a ready-to-copy folder per architecture (see CMakePresets.json):
#   dist/<arch>/{winmm.dll, dinput.dll, dinput8.dll, SDL3.dll, dws_input.ini}
# ---------------------------------------------------------------------------
install(TARGETS dws_winmm dws_dinput dws_dinput8 RUNTIME DESTINATION .)
install(FILES dws_input.ini DESTINATION .)

get_target_property(_dws_sdl_type SDL3::SDL3 TYPE)
if(_dws_sdl_type STREQUAL "SHARED_LIBRARY")
    install(FILES $<TARGET_FILE:SDL3::SDL3> DESTINATION .)
endif()

# ---------------------------------------------------------------------------
# Stage next to the DLLs after every build, so build/<arch>/bin/<Config>/ is a
# ready-to-test folder: winmm.dll, dinput.dll, dinput8.dll, SDL3.dll, dws_input.ini.
# Edit the master dws_input.ini in the source tree; the staged copy is refreshed
# whenever it differs.
# ---------------------------------------------------------------------------
add_custom_target(dws_stage ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/dws_input.ini" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    COMMENT "Staging dws_input.ini into bin/$<CONFIG>"
    VERBATIM)
add_dependencies(dws_stage dws_winmm dws_dinput dws_dinput8)

if(_dws_sdl_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET dws_stage POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        COMMENT "Staging SDL3.dll into bin/$<CONFIG>"
        VERBATIM)
endif()
