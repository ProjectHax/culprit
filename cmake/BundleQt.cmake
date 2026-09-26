# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Install-time script (see Packaging.cmake): copies the Qt plugins Culprit
# needs and every Qt library they and Culprit load into
# <libdir>/culprit/{lib,plugins}, then writes THIRD-PARTY-BUNDLED.txt with the
# licenses of everything that was copied.

# file(INSTALL) adds $DESTDIR itself; reads need it spelled out.
set(bundle_dest "${CMAKE_INSTALL_PREFIX}/${CULPRIT_BUNDLE_DIR}")
set(bundle "$ENV{DESTDIR}${bundle_dest}")
set(doc_dir "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${CULPRIT_DOC_DIR}")

# Platform integration (X11, Wayland, headless), the desktop portal, input
# methods, and SVG for the theme's icons.
set(plugin_patterns
  platforms/libqxcb.so
  platforms/libqwayland*.so
  platforms/libqoffscreen.so
  platformthemes/libqxdgdesktopportal.so
  platforminputcontexts/*.so
  xcbglintegrations/*.so
  wayland-shell-integration/*.so
  wayland-decoration-client/*.so
  wayland-graphics-integration-client/*.so
  imageformats/libqsvg.so
  iconengines/libqsvgicon.so)

set(modules "")
foreach(pattern IN LISTS plugin_patterns)
  file(GLOB found "${CULPRIT_QT_PLUGINDIR}/${pattern}")
  foreach(plugin IN LISTS found)
    file(RELATIVE_PATH rel "${CULPRIT_QT_PLUGINDIR}" "${plugin}")
    get_filename_component(subdir "${rel}" DIRECTORY)
    file(INSTALL "${plugin}" DESTINATION "${bundle_dest}/plugins/${subdir}")
    list(APPEND modules "${plugin}")
  endforeach()
endforeach()
if(NOT modules MATCHES "libqxcb")
  message(FATAL_ERROR "No Qt platform plugins found in ${CULPRIT_QT_PLUGINDIR}")
endif()

# Only libraries from the Qt installation are bundled (Qt and its ICU); system
# libraries become package dependencies. The build-tree executable is scanned:
# its RPATH still points at Qt.
# Plugins reach Qt through $ORIGIN/../../lib, so one library can resolve to
# several spellings of the same path; CMake reports those as conflicts. Match on
# the Qt prefix, take both lists and normalise.
string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" qt_prefix_re "${CULPRIT_QT_PREFIX}")
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${CULPRIT_EXECUTABLE}"
  MODULES ${modules}
  DIRECTORIES "${CULPRIT_QT_LIBDIR}"
  RESOLVED_DEPENDENCIES_VAR deps
  UNRESOLVED_DEPENDENCIES_VAR missing
  CONFLICTING_DEPENDENCIES_PREFIX conflict
  POST_INCLUDE_REGEXES "^${qt_prefix_re}/"
  POST_EXCLUDE_REGEXES ".*")
foreach(name IN LISTS conflict_FILENAMES)
  foreach(candidate IN LISTS conflict_${name})
    if(candidate MATCHES "^${qt_prefix_re}/")
      list(APPEND deps "${candidate}")
      break()
    endif()
  endforeach()
endforeach()
if(missing)
  message(WARNING "Libraries not found on the build machine (they must come from the target system): ${missing}")
endif()
set(libs "")
foreach(dep IN LISTS deps)
  get_filename_component(dep "${dep}" ABSOLUTE)
  list(APPEND libs "${dep}")
endforeach()
list(REMOVE_DUPLICATES libs)
foreach(lib IN LISTS libs)
  file(INSTALL "${lib}" DESTINATION "${bundle_dest}/lib" FOLLOW_SYMLINK_CHAIN)
endforeach()

set(notices_args --bundle "${bundle}" --qt-prefix "${CULPRIT_QT_PREFIX}"
  --output "${doc_dir}/THIRD-PARTY-BUNDLED.txt")
foreach(dir IN LISTS CULPRIT_QT_LICENSE_DIRS)
  list(APPEND notices_args --qt-licenses "${dir}")
endforeach()
foreach(entry IN LISTS CULPRIT_EXTRA_LICENSES)
  list(APPEND notices_args --license "${entry}")
endforeach()
if(CULPRIT_QT_LICENSE_DIRS)
  list(APPEND notices_args --strict)   # license texts were provided: all must be found
endif()
execute_process(COMMAND "${CULPRIT_PYTHON}" "${CULPRIT_NOTICES_SCRIPT}" ${notices_args}
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "bundle-notices.py failed (${rc})")
endif()
