cmake_minimum_required(VERSION 3.20)

# THE version of the app. Bump this in the same commit you tag; the release
# workflow refuses to publish if the git tag and this line disagree.
#
# Deliberately a plain variable, not a CACHE one: a cached version sticks to
# whatever an existing build directory was first configured with, so editing
# this line would silently do nothing and the binary would keep reporting a
# stale version (and keep offering itself an update forever).
set(DCA_VERSION "1.2.1")
project(DesktopCamApp VERSION ${DCA_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "DesktopCamApp is a Windows-only project.")
endif()

# Where the built-in updater looks for new releases.
set(DCA_GITHUB_OWNER "KaikoClanworth1" CACHE STRING "GitHub account hosting the releases")
set(DCA_GITHUB_REPO  "DesktopCamApp"   CACHE STRING "GitHub repository name")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h
    @ONLY
)

# --- Dear ImGui (fetched) -----------------------------------------------------
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# --- Application --------------------------------------------------------------
add_executable(DesktopCamApp WIN32
    src/main.cpp
    src/Application.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/VideoCapture.cpp
    src/AudioEngine.cpp
    src/UI.cpp
    src/DebugConsole.cpp
    src/Settings.cpp
    src/UpscalerNV.cpp
    src/Updater.cpp
    src/app.rc
)

# The .rc references icon.ico via a relative path; tell the resource
# compiler where to find it.
set_property(SOURCE src/app.rc PROPERTY COMPILE_FLAGS "/I\"${CMAKE_SOURCE_DIR}\"")

target_include_directories(DesktopCamApp PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(DesktopCamApp PRIVATE
    imgui
    d3d11
    dxgi
    d3dcompiler
    dxguid
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    oleaut32
    uuid
    winmm
    propsys
    avrt
    ksuser
    winhttp
    shell32
)

target_compile_definitions(DesktopCamApp PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(DesktopCamApp PRIVATE /W3 /permissive- /Zc:__cplusplus /MP)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DesktopCamApp)
