# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# culprit_common: pure C++20 (no Qt) — shared by the GUI core and the root helper.
add_library(culprit_common STATIC
  fs/File.cpp
  fs/File.h
  parse/PidParsers.cpp
  parse/PidParsers.h
  parse/SystemParsers.cpp
  parse/SystemParsers.h
  parse/Text.h
  trace/TraceFormat.cpp
  trace/TraceFormat.h
  util/Clock.h
  util/JsonWriter.h
  util/SpscRing.h
)

target_include_directories(culprit_common PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(culprit_common PUBLIC Threads::Threads)
set_target_properties(culprit_common PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
