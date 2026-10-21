// Functions only DeskDash.ino calls, to feed board state into the core.
#pragma once
#include "deskdash.h"

void core_init_lock();
void core_set_power(const PowerState &p);
void core_set_indoor(float temp_c, float humidity);
void core_load_brightness();
