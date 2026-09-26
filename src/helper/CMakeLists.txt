# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# culprit-helper: privileged tracing helper, launched via pkexec. No Qt.
add_executable(culprit-helper
  main.cpp
  PerfTracer.cpp
  PerfTracer.h
  PrivProc.cpp
  PrivProc.h
  SchedTracker.cpp
  SchedTracker.h
)

target_link_libraries(culprit-helper PRIVATE culprit_common)
set_target_properties(culprit-helper PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# Runs as root: harden it.
target_compile_options(culprit-helper PRIVATE -fstack-protector-strong -D_FORTIFY_SOURCE=3)
target_link_options(culprit-helper PRIVATE -pie -Wl,-z,relro,-z,now)

install(TARGETS culprit-helper RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
