# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# culprit: Qt Widgets GUI (+ `--record` headless recorder mode).
qt_add_executable(culprit
  main.cpp
  AboutDialog.cpp
  AboutDialog.h
  Dialogs.cpp
  Dialogs.h
  MainWindow.cpp
  MainWindow.h
  SettingsDialog.cpp
  SettingsDialog.h
  SystemAppearance.cpp
  SystemAppearance.h
  ThemeManager.cpp
  ThemeManager.h
  Theme.cpp
  Theme.h
  models/ProcessModel.cpp
  models/ProcessModel.h
  tabs/LoadIoTab.cpp
  tabs/LoadIoTab.h
  tabs/OverviewTab.cpp
  tabs/OverviewTab.h
  tabs/ProcessesTab.cpp
  tabs/ProcessesTab.h
  tabs/RecordingsTab.cpp
  tabs/RecordingsTab.h
  tabs/StutterTab.cpp
  tabs/StutterTab.h
  tabs/TabPage.h
  tabs/ThermalsTab.cpp
  tabs/ThermalsTab.h
  widgets/CpuHeatmap.cpp
  widgets/CpuHeatmap.h
  widgets/DataTable.cpp
  widgets/DataTable.h
  widgets/FindingsPanel.cpp
  widgets/HeaderFit.cpp
  widgets/HeaderFit.h
  widgets/HitchDetail.cpp
  widgets/HitchDetail.h
  widgets/LayoutBarrier.cpp
  widgets/LayoutBarrier.h
  widgets/FindingsPanel.h
  widgets/Sparkline.cpp
  widgets/Sparkline.h
  widgets/TimelineView.cpp
  widgets/TimelineView.h
  widgets/TitleBar.cpp
  widgets/TitleBar.h
  widgets/WindowChrome.cpp
  widgets/WindowChrome.h
)

target_link_libraries(culprit PRIVATE culprit_core Qt6::Widgets Qt6::Gui)

qt_add_resources(culprit "theme_assets"
  PREFIX "/theme"
  BASE resources/theme
  FILES
    resources/theme/check.svg
    resources/theme/light/chevron-down.svg
    resources/theme/light/chevron-up.svg
    resources/theme/dark/chevron-down.svg
    resources/theme/dark/chevron-up.svg)

# Query the Wayland compositor for server-side decorations (optional).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
endif()
if(WAYLAND_CLIENT_FOUND)
  target_link_libraries(culprit PRIVATE PkgConfig::WAYLAND_CLIENT)
  target_compile_definitions(culprit PRIVATE CULPRIT_HAVE_WAYLAND_CLIENT)
endif()

# Culprit's license and the third-party notices, shown in the About dialog.
qt_add_resources(culprit "legal"
  PREFIX "/legal"
  BASE ${PROJECT_SOURCE_DIR}
  FILES
    ${PROJECT_SOURCE_DIR}/LICENSE
    ${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
    ${PROJECT_SOURCE_DIR}/licenses/LGPL-3.0-only.txt)

# Where packages that bundle Qt put THIRD-PARTY-BUNDLED.txt, relative to the binary.
file(RELATIVE_PATH culprit_docdir_from_bindir "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DOCDIR}")
target_compile_definitions(culprit PRIVATE CULPRIT_DOCDIR_FROM_BINDIR="${culprit_docdir_from_bindir}")

qt_add_resources(culprit "app_icon"
  PREFIX "/"
  BASE ${PROJECT_SOURCE_DIR}/packaging
  FILES ${PROJECT_SOURCE_DIR}/packaging/culprit.svg)

if(CULPRIT_BUNDLE_QT)
  # The private Qt copy (see cmake/BundleQt.cmake) is found through RUNPATH and an
  # embedded qt.conf; both are relative, so the package stays relocatable.
  file(RELATIVE_PATH culprit_bundle_rel "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/culprit")
  set_target_properties(culprit PROPERTIES INSTALL_RPATH "$ORIGIN/${culprit_bundle_rel}/lib")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qtconf/qt.conf" "[Paths]\nPrefix = ${culprit_bundle_rel}\n")
  qt_add_resources(culprit "qt_conf"
    PREFIX "/qt/etc"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/qtconf"
    FILES "${CMAKE_CURRENT_BINARY_DIR}/qtconf/qt.conf")
endif()

install(TARGETS culprit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
