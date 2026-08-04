// game2048.h - swipe-driven 2048 on the badge panel.
#pragma once

#include <Arduino_GFX_Library.h>
#include "types.h"

void game2048Reset();
bool game2048HandleGesture(Gesture g, int tapX, int tapY);
void drawGame2048(Arduino_GFX *g, int ox, int oy);
