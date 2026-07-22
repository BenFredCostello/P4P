#pragma once
#include <Arduino.h>

// I2S capture for mic 1 (I2S port 0). 16kHz mono, delivered as 16-bit PCM.

bool micInit();

// Reads up to maxSamples int16 samples into `out`. Returns the number of
// samples actually read (0 if none were ready). Safe to call every loop().
size_t micReadChunk(int16_t *out, size_t maxSamples);
