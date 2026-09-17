#pragma once

#include <Arduino.h>

// Initialise the OLED on the XIAO's I2C pins. Returns false if absent.
bool displayInit();

// Draw one final transcript. Call from loop(), not a BLE callback.
void displayShowSentence(const String& sentence);
