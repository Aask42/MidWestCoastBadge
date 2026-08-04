#pragma once

#include <Arduino.h>

void bleBegin();
void bleTick(uint32_t now);

uint8_t bleNearbyCount();
uint16_t bleSessionCount();
uint8_t blePeerCount();
const char *blePeerId(uint8_t index);
int8_t blePeerRssi(uint8_t index);
uint32_t blePeerAgeMs(uint8_t index);