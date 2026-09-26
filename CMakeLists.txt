# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

cmake_minimum_required(VERSION 3.21)

project(Culprit
  VERSION 0.1.0
  DESCRIPTION "Linux diagnostic monitor: finds the culprit behind load, heat and stutter"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(Parallelism)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

# Documentation goes to share/doc/culprit, named like the packages.
set(CMAKE_INSTALL_DOCDIR "share/doc/culprit" CACHE PATH "Documentation directory")
include(GNUInstallDirs)

option(CULPRIT_BUNDLE_QT
  "Install a private copy of the Qt libraries and plugins (for packages built against Qt's official binaries)" OFF)

find_package(Threads REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets DBus)
qt_standard_project_setup()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(src/common)
add_subdirectory(src/helper)
add_subdirectory(src/core)
add_subdirectory(src/gui)

# polkit action for the helper (names the installed, root-owned helper path)
set(CULPRIT_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/culprit-helper")
configure_file(packaging/org.culprit.helper.policy.in ${PROJECT_BINARY_DIR}/org.culprit.helper.policy @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/org.culprit.helper.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)

# desktop integration and the background recorder (systemd user unit)
set(CULPRIT_BIN_PATH "${CMAKE_INSTALL_FULL_BINDIR}/culprit")
set(CULPRIT_DOC_PATH "${CMAKE_INSTALL_FULL_DOCDIR}")
configure_file(packaging/culprit-recorder.service.in ${PROJECT_BINARY_DIR}/culprit-recorder.service @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/culprit-recorder.service DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)
install(FILES packaging/culprit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/culprit.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES README.md LICENSE THIRD-PARTY-NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY licenses tools/induce DESTINATION ${CMAKE_INSTALL_DOCDIR} USE_SOURCE_PERMISSIONS)

include(Packaging)
