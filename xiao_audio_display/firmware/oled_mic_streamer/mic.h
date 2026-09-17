#pragma once

#include <Arduino.h>

// Audio sample rate expected by Deepgram.
#define SAMPLE_RATE 16000

// 90 PCM16 samples = 180 bytes.
//
// At 16 kHz:
// 90 / 16000 = 5.625 ms of audio.
//
// 180 bytes also fits neatly inside the BLE
// audio payload we're currently using.
#define BUFFER_SAMPLES 90

// Initialise the INMP441 / I2S peripheral.
void micInit();

// Read one chunk from the microphone.
//
// output:
//     destination PCM16 mono buffer
//
// maxSamples:
//     capacity of output buffer
//
// peak:
//     optional peak amplitude output
//
// Returns:
//     number of PCM16 samples written into output
size_t micReadChunk(int16_t* output, size_t maxSamples, int32_t* peak);