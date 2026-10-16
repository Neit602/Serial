cmake_minimum_required(VERSION 3.21)

project(Portside
    VERSION 0.2.0
    DESCRIPTION "Cross-platform serial terminal for Linux, macOS and Windows"
    HOMEPAGE_URL "https://github.com/portside/portside"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PORTSIDE_BUILD_GUI "Build the portside-gui desktop application" ON)
option(PORTSIDE_BUILD_TESTS "Build the test suite" ON)
option(PORTSIDE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# The core and the CLI need only QtCore and QtSerialPort; the desktop app adds
# QtWidgets.  Keeping Widgets out of the core is what lets `portside` run on a
# headless lab box with no GUI libraries installed (DECISIONS D35).
# 6.6 for QtFuture::makeReadyValueFuture and friends.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core SerialPort)
if(PORTSIDE_BUILD_GUI)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Gui Widgets)
endif()
if(PORTSIDE_BUILD_TESTS)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)
endif()

add_library(portside_warnings INTERFACE)
if(MSVC)
    target_compile_options(portside_warnings INTERFACE /W4 /permissive- /utf-8)
    if(PORTSIDE_WARNINGS_AS_ERRORS)
        target_compile_options(portside_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(portside_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(PORTSIDE_WARNINGS_AS_ERRORS)
        target_compile_options(portside_warnings INTERFACE -Werror)
    endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/cli)
if(PORTSIDE_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(PORTSIDE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS portside RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(PORTSIDE_BUILD_GUI)
    install(TARGETS portside-gui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        BUNDLE DESTINATION .)
endif()
