// Copyright (c) 2012- PPSSPP Project.
// Adapted from Dolphin-NX.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void saltynx_start(void);
void saltynx_stop(void);
void saltynx_set_resolution(uint32_t width, uint32_t height);
void saltynx_frame(uint8_t api);

#ifdef __cplusplus
}
#endif
