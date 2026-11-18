# Copyright (c) 2012- PPSSPP Project.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The Switch 2 run-time shader compiler in place of uam: uam2rt (drako3d's UAM2) from the Switch 2 Mesa SDK, behind
# UamBridge.h (Common/GPU/Deko3D/UamBridgeSwitch2.cpp).

find_package(uam2rt CONFIG REQUIRED)
message(STATUS "Switch 2 shader compiler: uam2rt ${uam2rt_VERSION}")

add_library(uam INTERFACE)
target_link_libraries(uam INTERFACE uam2rt::uam2rt)

# lsfg-deko3d versions its shader cache with this (the Switch's is uam's commit).
set(SWITCH_UAM2RT_REVISION "uam2rt-${uam2rt_VERSION}")
