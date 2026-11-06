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

if (ANDROID)
    target_link_libraries(huxerui_media PRIVATE mediandk dl)
endif ()

if (WIN32)
    target_compile_definitions(huxerui_media PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(huxerui_media PRIVATE mfplat mfuuid mf d3d11 dxgi oleaut32 ole32 wmcodecdspuuid)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MEDIA_GTK REQUIRED IMPORTED_TARGET "gtk4>=4.14")
    target_link_libraries(huxerui_media PRIVATE PkgConfig::MEDIA_GTK)
    option(HUXERUI_MEDIA_GSTREAMER "Enable Linux AAC/AVC codecs using installed GStreamer plugins" OFF)
    if (HUXERUI_MEDIA_GSTREAMER)
        pkg_check_modules(MEDIA_GSTREAMER REQUIRED IMPORTED_TARGET
                "gstreamer-app-1.0>=1.24" "gstreamer-audio-1.0>=1.24" "gstreamer-video-1.0>=1.24")
        target_link_libraries(huxerui_media PRIVATE PkgConfig::MEDIA_GSTREAMER)
        target_compile_definitions(huxerui_media PRIVATE HUXERUI_MEDIA_GSTREAMER=1)
    endif ()
endif ()

if (EMSCRIPTEN)
    set_property(TARGET huxerui_media APPEND PROPERTY INTERFACE_LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/codec.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/playback.js")
    target_link_options(huxerui_media INTERFACE
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/media.js\""
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/codec.js\""
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform/web/playback.js\""
            "--bind"
    )
endif ()

if (APPLE)
    target_link_libraries(huxerui_media PRIVATE "-framework AVFoundation" "-framework QuartzCore" "-framework CoreMedia" "-framework AudioToolbox" "-framework VideoToolbox" "-framework CoreVideo")
    if (IOS)
        target_link_libraries(huxerui_media PRIVATE "-framework Photos" "-framework PhotosUI" "-framework UniformTypeIdentifiers")
    endif ()
endif ()

option(HUXERUI_MEDIA_BUILD_TESTS "Build Media contract tests" OFF)
if (HUXERUI_MEDIA_BUILD_TESTS AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_testing()
    add_subdirectory(tests)
endif ()
