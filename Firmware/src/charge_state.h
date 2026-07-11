#pragma once
#include <stdint.h>

// Poll PC1 charge-status pin once per second; when the level changes,
// notify "CHG:0" (not charging) or "CHG:1" (charging) over BLE log so the
// web control page can react in real time.  Initial state is "unknown"
// so the first tick always emits the current level.
void charge_state_tick(void);