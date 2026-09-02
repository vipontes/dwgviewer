cmake_minimum_required(VERSION 3.16)
project(dwgviewer_prototype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# --- libdxfrw -----------------------------------------------------------
# We deliberately do NOT add_subdirectory() libdxfrw's own CMakeLists.txt:
# that one also builds the dwg2dxf CLI tool, doc target, and an
# install/export config step (which is currently broken in this vendored
# copy — it references a cmake/libdxfrwConfig.cmake.in that doesn't exist
# in the repo yet, see LibreCAD/LibreCAD's libraries/libdxfrw). None of
# that is relevant to us anyway: we only want the `dxfrw` static library.
#
# Instead we pull in libdxfrw_sources.cmake, which is a clean, upstream-
# maintained manifest of just the library's source/header files, and
# build our own minimal target from it. This is the standard pattern for
# vendoring a subproject you don't want the packaging boilerplate of.
include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdxfrw/libdxfrw_sources.cmake")

add_library(dxfrw STATIC ${LIBDXFRW_SOURCES} ${LIBDXFRW_PRIVATE_HEADERS} ${LIBDXFRW_PUBLIC_HEADERS})
target_compile_features(dxfrw PUBLIC cxx_std_17)
target_include_directories(dxfrw PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdxfrw/src"
)

# --- Qt -------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(dwgviewer
    src/main.cpp
    src/dwg_document.cpp
    src/dwg_document.h
    src/viewer_widget.cpp
    src/viewer_widget.h
)
target_link_libraries(dwgviewer PRIVATE Qt6::Widgets dxfrw)

if(WIN32)
    # Default MSVC subsystem is Console, which pops a console window
    # alongside the GUI on every launch (including double-click from
    # Explorer). WIN32_EXECUTABLE switches to the Windows subsystem instead;
    # main.cpp re-attaches to the parent console when launched from one
    # (cmd/PowerShell), so stderr diagnostics and --png output still show up
    # there.
    set_target_properties(dwgviewer PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
