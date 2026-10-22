cmake_minimum_required(VERSION 3.22)

project(lod_poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Mod metadata (mirrors manifest.json)
# ---------------------------------------------------------------------------

set(MOD_ID "lod_poc" CACHE STRING "LeviLauncher mod package directory name")
set(MOD_LIBRARY_NAME "lod_poc" CACHE STRING "Native library name without lib prefix or .so suffix")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

include(FetchContent)

if(ANDROID)
    FetchContent_Declare(
        preloader_android
        GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
        GIT_TAG 5d108da350ce3e7f13f51415f81e65344ecd5d14)
else()
    FetchContent_Declare(
        preloader_android
        GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
        GIT_TAG 5d108da350ce3e7f13f51415f81e65344ecd5d14
        SOURCE_SUBDIR cmake/host-sdk)
endif()

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_Declare(
    boost_pfr
    GIT_REPOSITORY https://github.com/boostorg/pfr.git
    GIT_TAG boost-1.88.0)
FetchContent_Declare(
    magic_enum
    GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
    GIT_TAG v0.9.7)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0)

set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)

FetchContent_MakeAvailable(nlohmann_json boost_pfr magic_enum fmt)
FetchContent_MakeAvailable(preloader_android)

FetchContent_GetProperties(preloader_android)
set(LEVI_PRELOADER_INCLUDE_DIR "${preloader_android_SOURCE_DIR}/include")

# ---------------------------------------------------------------------------
# Host build: typed config generator
# ---------------------------------------------------------------------------

if(NOT ANDROID)
    set(LEVI_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-config")

    add_executable(levi_config_generator
        src/config_generator.cpp
        src/mod/Config.cpp)

    target_include_directories(levi_config_generator
        PRIVATE
            src
            "${LEVI_PRELOADER_INCLUDE_DIR}")

    target_compile_definitions(levi_config_generator PRIVATE PL_CONFIG_NO_RUNTIME)

    target_link_libraries(levi_config_generator
        PRIVATE
            nlohmann_json::nlohmann_json
            Boost::pfr
            magic_enum::magic_enum)

    add_custom_target(levi_generate_config
        COMMAND levi_config_generator "${LEVI_GENERATED_CONFIG_DIR}"
        DEPENDS levi_config_generator
        COMMENT "Generating config.json and config.schema.json"
        VERBATIM)

    return()
endif()

# ---------------------------------------------------------------------------
# Android build
# ---------------------------------------------------------------------------

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "LOD PoC only supports arm64-v8a, got: ${CMAKE_ANDROID_ARCH_ABI}")
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${CMAKE_ANDROID_ARCH_ABI}")

find_library(LL_LOG_LIB log REQUIRED)
find_library(LL_EGL_LIB EGL REQUIRED)
find_library(LL_GLES_LIB GLESv2 REQUIRED)
find_library(LL_Z_LIB z REQUIRED)

add_library(${MOD_LIBRARY_NAME} SHARED
    src/main.cpp
    src/mod/CameraProbe.cpp
    src/mod/Config.cpp
    src/mod/ElfDiag.cpp
    src/mod/FileLog.cpp
    src/mod/GlTrace.cpp
    src/mod/LdbReader.cpp
    src/mod/LodMesh.cpp
    src/mod/LodPocMod.cpp
    src/mod/PngWriter.cpp
    src/mod/PocQuad.cpp
    src/mod/WorldReader.cpp)

set_target_properties(${MOD_LIBRARY_NAME} PROPERTIES
    OUTPUT_NAME "${MOD_LIBRARY_NAME}")

target_include_directories(${MOD_LIBRARY_NAME}
    PRIVATE
        src
        "${LEVI_PRELOADER_INCLUDE_DIR}")

target_compile_options(${MOD_LIBRARY_NAME}
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden -fvisibility-inlines-hidden>)

target_link_options(${MOD_LIBRARY_NAME}
    PRIVATE
        "-Wl,-z,max-page-size=16384")

target_link_libraries(${MOD_LIBRARY_NAME}
    PRIVATE
        ${LL_LOG_LIB}
        ${LL_EGL_LIB}
        ${LL_GLES_LIB}
        ${LL_Z_LIB}
        nlohmann_json::nlohmann_json
        Boost::pfr
        magic_enum::magic_enum
        fmt::fmt)

if(TARGET preloader)
    target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE preloader)
else()
    target_link_options(${MOD_LIBRARY_NAME}
        PRIVATE
            "-Wl,--unresolved-symbols=ignore-all")
endif()

if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "CMAKE_STRIP is required to strip the generated library.")
endif()

add_custom_command(TARGET ${MOD_LIBRARY_NAME} POST_BUILD
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
    COMMENT "Stripping symbols from $<TARGET_FILE_NAME:${MOD_LIBRARY_NAME}>"
    VERBATIM)
