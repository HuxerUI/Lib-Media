cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(huxerui_media VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE)
    if (IOS)
        set(LIBRARY_OBJCXX_PLATFORM ios)
    else ()
        set(LIBRARY_OBJCXX_PLATFORM macos)
    endif ()
    file(GLOB_RECURSE LIBRARY_OBJCXX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/${LIBRARY_OBJCXX_PLATFORM}/src/*.mm"
    )
    set_source_files_properties(${LIBRARY_OBJCXX_SOURCE_FILES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_OBJCXX_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_media
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            media
)

add_library(HuxerUI::Media ALIAS huxerui_media)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_media PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_include_directories(huxerui_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

option(HUXERUI_MEDIA_ENABLE_AOM "Use libaom for CPU AV1 encoding and ten-bit decoding when available" OFF)
if (HUXERUI_MEDIA_ENABLE_AOM AND NOT ANDROID AND NOT IOS AND NOT EMSCRIPTEN)
    find_path(MEDIA_AOM_INCLUDE_DIR aom/aom_encoder.h)
    find_library(MEDIA_AOM_LIBRARY aom)
    if (MEDIA_AOM_INCLUDE_DIR AND MEDIA_AOM_LIBRARY)
        target_include_directories(huxerui_media PRIVATE "${MEDIA_AOM_INCLUDE_DIR}")
        target_link_libraries(huxerui_media PRIVATE "${MEDIA_AOM_LIBRARY}")
        target_compile_definitions(huxerui_media PRIVATE MEDIA_HAS_AOM=1)
    else ()
        message(FATAL_ERROR "HUXERUI_MEDIA_ENABLE_AOM requires the corresponding library and headers for the target")
    endif ()
endif ()

option(HUXERUI_MEDIA_ENABLE_OPUS "Use libopus for mapping-family-one Opus decoding when available" OFF)
if (HUXERUI_MEDIA_ENABLE_OPUS AND NOT ANDROID AND NOT IOS AND NOT EMSCRIPTEN)
    find_path(MEDIA_OPUS_INCLUDE_DIR opus/opus_multistream.h)
    find_library(MEDIA_OPUS_LIBRARY opus)
    if (MEDIA_OPUS_INCLUDE_DIR AND MEDIA_OPUS_LIBRARY)
        target_include_directories(huxerui_media PRIVATE "${MEDIA_OPUS_INCLUDE_DIR}")
        target_link_libraries(huxerui_media PRIVATE "${MEDIA_OPUS_LIBRARY}")
        target_compile_definitions(huxerui_media PRIVATE MEDIA_HAS_OPUS=1)
    else ()
        message(FATAL_ERROR "HUXERUI_MEDIA_ENABLE_OPUS requires the corresponding library and headers for the target")
    endif ()
endif ()

option(HUXERUI_MEDIA_ENABLE_FLAC_ENCODER "Enable PCM encoding with target-compatible libFLAC" OFF)
option(HUXERUI_MEDIA_ENABLE_MP3_ENCODER "Enable PCM encoding with target-compatible libmp3lame" OFF)
foreach (codec IN ITEMS FLAC MP3)
    if (NOT HUXERUI_MEDIA_ENABLE_${codec}_ENCODER)
        continue()
    endif ()
    if (codec STREQUAL "FLAC")
        if (NOT MEDIA_FLAC_ENCODER_INCLUDE_DIR AND NOT MEDIA_FLAC_ENCODER_LIBRARY)
            find_package(FLAC CONFIG QUIET)
            if (TARGET FLAC::FLAC)
                target_link_libraries(huxerui_media PRIVATE FLAC::FLAC)
                target_compile_definitions(huxerui_media PRIVATE MEDIA_HAS_FLAC_ENCODER=1)
                continue()
            endif ()
        endif ()
        find_path(MEDIA_FLAC_ENCODER_INCLUDE_DIR FLAC/stream_encoder.h)
        find_library(MEDIA_FLAC_ENCODER_LIBRARY FLAC)
    else ()
        find_path(MEDIA_MP3_ENCODER_INCLUDE_DIR lame/lame.h)
        find_library(MEDIA_MP3_ENCODER_LIBRARY mp3lame)
    endif ()
    if (NOT MEDIA_${codec}_ENCODER_INCLUDE_DIR OR NOT MEDIA_${codec}_ENCODER_LIBRARY)
        message(FATAL_ERROR "HUXERUI_MEDIA_ENABLE_${codec}_ENCODER requires the library and headers for the target")
    endif ()
    target_include_directories(huxerui_media PRIVATE "${MEDIA_${codec}_ENCODER_INCLUDE_DIR}")
    target_link_libraries(huxerui_media PRIVATE ${MEDIA_${codec}_ENCODER_LIBRARY})
    target_compile_definitions(huxerui_media PRIVATE MEDIA_HAS_${codec}_ENCODER=1)
endforeach ()

enable_language(C)
file(GLOB VORBIS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vorbis/lib/*.c")
target_sources(huxerui_media PRIVATE ${VORBIS_SOURCES} third_party/ogg/bitwise.c)
target_include_directories(huxerui_media PRIVATE third_party/vorbis/include third_party/ogg/include)
target_compile_features(huxerui_media PRIVATE c_std_99)
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${VORBIS_SOURCES} third_party/ogg/bitwise.c PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
endif ()
if (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(huxerui_media PRIVATE m)
endif ()

if (ANDROID)
    target_link_libraries(huxerui_media PRIVATE mediandk android dl EGL GLESv3)
endif ()

if (WIN32)
    target_compile_definitions(huxerui_media PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(huxerui_media PRIVATE mfplat mfuuid mf d3d11 dxgi oleaut32 ole32 wmcodecdspuuid)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MEDIA_GTK REQUIRED IMPORTED_TARGET "gtk4>=4.14")
    pkg_check_modules(MEDIA_PULSEAUDIO REQUIRED IMPORTED_TARGET libpulse)
    pkg_check_modules(MEDIA_GSTREAMER REQUIRED IMPORTED_TARGET
            "gstreamer-app-1.0>=1.24" "gstreamer-audio-1.0>=1.24" "gstreamer-video-1.0>=1.24")
    target_link_libraries(huxerui_media PRIVATE
            PkgConfig::MEDIA_GTK PkgConfig::MEDIA_PULSEAUDIO PkgConfig::MEDIA_GSTREAMER)
endif ()

if (EMSCRIPTEN)
    set_property(TARGET huxerui_media APPEND PROPERTY INTERFACE_LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media_codec_decoder.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media_codec_encoder.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media_codec_capabilities.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/playback.js")
    target_link_options(huxerui_media INTERFACE
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media.js\""
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media_codec_decoder.js\""
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media_codec_encoder.js\""
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media_codec_capabilities.js\""
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/playback.js\""
            "--bind"
    )
endif ()

if (APPLE)
    target_link_libraries(huxerui_media PRIVATE "-framework AVFoundation" "-framework QuartzCore" "-framework CoreMedia" "-framework AudioToolbox" "-framework VideoToolbox" "-framework CoreVideo" "-framework Metal")
    if (IOS)
        target_link_libraries(huxerui_media PRIVATE "-framework Photos" "-framework PhotosUI" "-framework UniformTypeIdentifiers")
    endif ()
endif ()

option(HUXERUI_MEDIA_BUILD_TESTS "Build Media contract tests" OFF)
if (HUXERUI_MEDIA_BUILD_TESTS AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_testing()
    add_subdirectory(tests)
endif ()
