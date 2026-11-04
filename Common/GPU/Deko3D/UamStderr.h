// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <stdio.h>

static inline FILE *UamGetSystemStderr(void) { return stderr; }

#ifdef stderr
#undef stderr
#endif

#ifdef __cplusplus
extern "C" {
#endif

FILE *UamGetStderr(void);

#ifdef __cplusplus
}
#endif

#define stderr UamGetStderr()
