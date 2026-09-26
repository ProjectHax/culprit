# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Caps build parallelism at half the logical CPU cores.
#
# build.sh already passes --parallel <nproc/2>, but these job pools make the cap
# hold for a bare `cmake --build` / `ninja` as well (Ninja generator only).

cmake_host_system_information(RESULT _culprit_cores QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR _culprit_half "${_culprit_cores} / 2")
if(_culprit_half LESS 1)
  set(_culprit_half 1)
endif()

set(CULPRIT_JOBS "${_culprit_half}" CACHE STRING
    "Maximum parallel compile jobs (default: half the logical CPU cores)")

set_property(GLOBAL APPEND PROPERTY JOB_POOLS
  culprit_compile=${CULPRIT_JOBS}
  culprit_link=2)
set(CMAKE_JOB_POOL_COMPILE culprit_compile)
set(CMAKE_JOB_POOL_LINK culprit_link)

# AUTOMOC/AUTOUIC spawn their own parallel moc/uic processes.
set(CMAKE_AUTOGEN_PARALLEL ${CULPRIT_JOBS})

message(STATUS "Culprit: limiting build to ${CULPRIT_JOBS} parallel jobs (${_culprit_cores} logical cores)")
