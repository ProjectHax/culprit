# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Package metadata for `cpack -G DEB` and `cpack -G RPM`, and (with
# CULPRIT_BUNDLE_QT) the install step that copies Qt into the package.
# packaging/ci/package.sh drives both, as do the GitHub Actions workflows.

if(CULPRIT_BUNDLE_QT)
  set(CULPRIT_QT_LICENSE_DIRS "" CACHE STRING
      "Directories with the license texts Qt's components use (the LICENSES folders of the Qt source packages)")
  set(CULPRIT_EXTRA_LICENSES "" CACHE STRING
      "NAME=FILE license texts for other bundled libraries, e.g. ICU=/path/to/icu/LICENSE")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  install(CODE "
    set(CULPRIT_QT_PREFIX \"${QT6_INSTALL_PREFIX}\")
    set(CULPRIT_QT_LIBDIR \"${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}\")
    set(CULPRIT_QT_PLUGINDIR \"${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}\")
    set(CULPRIT_BUNDLE_DIR \"${CMAKE_INSTALL_LIBDIR}/culprit\")
    set(CULPRIT_EXECUTABLE \"$<TARGET_FILE:culprit>\")
    set(CULPRIT_DOC_DIR \"${CMAKE_INSTALL_DOCDIR}\")
    set(CULPRIT_PYTHON \"${Python3_EXECUTABLE}\")
    set(CULPRIT_NOTICES_SCRIPT \"${PROJECT_SOURCE_DIR}/packaging/bundle-notices.py\")
    set(CULPRIT_QT_LICENSE_DIRS \"${CULPRIT_QT_LICENSE_DIRS}\")
    set(CULPRIT_EXTRA_LICENSES \"${CULPRIT_EXTRA_LICENSES}\")
  ")
  install(SCRIPT cmake/BundleQt.cmake)
endif()

option(CULPRIT_DEBIAN_COPYRIGHT "Install packaging/debian/copyright (for the .deb)" OFF)
if(CULPRIT_DEBIAN_COPYRIGHT)
  install(FILES packaging/debian/copyright DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

set(CPACK_PACKAGE_NAME culprit)
set(CPACK_PACKAGE_VENDOR "ProjectHax LLC")
set(CPACK_PACKAGE_CONTACT "ProjectHax LLC <ryan@projecthax.com>")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux diagnostic monitor that finds the culprit behind load, heat and stutter")
set(CPACK_PACKAGE_DESCRIPTION
  "Culprit is a task manager built for diagnosis. It explains why the machine is
loaded, hot or stuttering and names the process, IRQ, device or kernel subsystem
responsible: run-queue wait per process, D-state threads by wait channel, power
and heat attribution, latency probes with a flight recorder for micro-stutters,
and an optional root helper that traces the scheduler for exact answers.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ProjectHax/culprit")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")

# .deb: dependencies come from dpkg-shlibdeps; the bundled Qt is private to the package.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pkexec | policykit-1, rtkit")
if(CULPRIT_BUNDLE_QT)
  # dpkg-shlibdeps runs in the staging directory, so this is relative to "/".
  string(REGEX REPLACE "^/" "" _culprit_prefix_rel "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${_culprit_prefix_rel}/${CMAKE_INSTALL_LIBDIR}/culprit/lib")
endif()

# .rpm: library dependencies are found automatically; Qt's SVG and Wayland
# plugins are loaded at run time, so they are listed explicitly.
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_REQUIRES "polkit, qt6-qtsvg, qt6-qtwayland")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  /usr/libexec
  /usr/share/applications
  /usr/share/icons
  /usr/share/icons/hicolor
  /usr/share/icons/hicolor/scalable
  /usr/share/icons/hicolor/scalable/apps
  /usr/share/polkit-1
  /usr/share/polkit-1/actions
  /usr/share/systemd
  /usr/share/systemd/user)

include(CPack)
