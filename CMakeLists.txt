cmake_minimum_required(VERSION 3.16)

project(KisakCOD)

# Set the default C++ standard
set(CMAKE_CXX_STANDARD 20)

# Set Address Sanitizer
# NOTE: If you use this you will need to copy to the game's directory
# clang_rt.asan_dbg_dynamic-i386.dll and clang_rt.asan_dynamic-i386.dll from the MSVC C directory located below (or similar)
# C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86
set(USE_ASAN OFF)

# ==========================================
# PHASE 1 PORTING TARGETS
# ==========================================
option(USE_OPENAL "Use OpenAL instead of proprietary Miles Sound System" ON)

if(USE_OPENAL)
    message(STATUS "AUDIO: Compiling with OpenAL backend.")
    add_definitions(-DUSE_OPENAL)
    
    # Point directly to our local deps folder instead of searching the host OS
    set(OPENAL_INCLUDE_DIR "${DEPS_DIR}/openal/include")
    set(OPENAL_LIBRARY_DIR "${DEPS_DIR}/openal/lib/Win32")
    set(OPENAL_LIBRARY "OpenAL32.lib")
endif()
# ==========================================

# Set the gloabl directory variables
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/scripts)

# Set the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR})

set_property( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${BIN_NAME} )
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /O2 /Ot /MP /W3 /Zi ${MSVC_WARNING_DISABLES} /permissive-")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd /Od /MP /W3 /Zi ${MSVC_WARNING_DISABLES} /permissive-")

# Include global scripts/tricks
include(${SCRIPTS_DIR}/extern/increment_build.cmake)

# Include the subprojects
add_subdirectory(${SCRIPTS_DIR}/mp)
add_subdirectory(${SCRIPTS_DIR}/sp)
add_subdirectory(${SCRIPTS_DIR}/dedi)