cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_media VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (IOS)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 20)
        set(IOS_FRAME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/decoded_preview_ios.mm")
        set_source_files_properties("${IOS_FRAME_SOURCE}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        list(APPEND APP_SOURCE_FILES "${IOS_FRAME_SOURCE}")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

huxerui_add_app(example_media
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "Media Preview"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.media.preview"
)

huxerui_configure_project_app(example_media)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    set_target_properties(example_media PROPERTIES CXX_EXTENSIONS OFF)
endif ()

huxerui_use_library(example_media
        TARGET HuxerUI::Media
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    target_include_directories(example_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/format")
    if (ANDROID OR APPLE OR EMSCRIPTEN OR WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(PREVIEW_FIXTURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/container-fixture")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/container/preview-avc-aac.mp4"
                "${PREVIEW_FIXTURE_ROOT}/raw/preview-avc-aac.mp4" COPYONLY)
        huxerui_add_resources(example_media ROOT "${PREVIEW_FIXTURE_ROOT}" NAMESPACE fixture)
    endif ()
    if (ANDROID)
        target_include_directories(example_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/android/src/main/cpp")
        target_link_libraries(example_media PRIVATE EGL GLESv3)
    elseif (IOS)
        target_include_directories(example_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/ios/src")
        target_link_libraries(example_media PRIVATE "-framework AVFoundation" "-framework CoreVideo")
    elseif (APPLE AND NOT IOS)
        target_include_directories(example_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/macos/src")
        target_link_libraries(example_media PRIVATE "-framework CoreVideo")
    elseif (WIN32)
        target_include_directories(example_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/windows/src")
        target_compile_definitions(example_media PRIVATE NOMINMAX)
    endif ()
endif ()
