cmake_minimum_required(VERSION 3.20)
project(MokaLoveReliveMotionSync LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(version SHARED
    src/main.cpp
    src/motion_input.cpp
    src/orgasm_state.cpp
    src/sr6_sync.cpp
    src/version_proxy.cpp
)

target_compile_definitions(version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/minhook/CMakeLists.txt")
    message(FATAL_ERROR "Vendored external/minhook is missing; restore the repository files.")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/minhook)
target_link_libraries(version PRIVATE minhook Comctl32 Winhttp UxTheme)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/generated_motion_curves.h")
    message(FATAL_ERROR "Generate motion curves from your own game first; see README.md or build.ps1 -GameRoot.")
endif()
