# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# culprit_core: collectors, stutter detection, analysis, recorder (QtCore only).
add_library(culprit_core STATIC
  Engine.cpp
  Engine.h
  Format.cpp
  Format.h
  Settings.cpp
  Settings.h
  Version.h
  actions/ProcessActions.cpp
  actions/ProcessActions.h
  analysis/Attribution.cpp
  analysis/Attribution.h
  analysis/RuleEngine.cpp
  analysis/RuleEngine.h
  analysis/rules/CpuRules.cpp
  analysis/rules/MemoryIoRules.cpp
  analysis/rules/Rules.h
  analysis/rules/SystemRules.cpp
  analysis/rules/ThermalRules.cpp
  collect/CgroupCollector.cpp
  collect/CgroupCollector.h
  collect/GpuCollector.cpp
  collect/GpuCollector.h
  collect/HwmonCollector.cpp
  collect/HwmonCollector.h
  collect/PowerCollector.cpp
  collect/PowerCollector.h
  collect/ProcessCollector.cpp
  collect/ProcessCollector.h
  collect/SystemCollector.cpp
  collect/SystemCollector.h
  helper/HelperClient.cpp
  helper/HelperClient.h
  model/Finding.h
  model/Frame.h
  model/Hitch.h
  model/Model.cpp
  stutter/FlightRecorder.cpp
  stutter/FlightRecorder.h
  stutter/HitchAnalyzer.cpp
  stutter/HitchAnalyzer.h
  stutter/LatencyProbe.cpp
  stutter/LatencyProbe.h
  record/Recorder.cpp
  record/Recorder.h
  record/RecordingReader.cpp
  record/RecordingReader.h
  record/Serialize.cpp
  record/Serialize.h
  report/ReportExporter.cpp
  report/ReportExporter.h
  report/TextReport.cpp
  report/TextReport.h
)

target_include_directories(culprit_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(culprit_core PUBLIC culprit_common Qt6::Core Qt6::DBus ${CMAKE_DL_LIBS})
target_compile_definitions(culprit_core PUBLIC
  CULPRIT_VERSION="${PROJECT_VERSION}"
  CULPRIT_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}")
