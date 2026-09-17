#pragma once

#include <Arduino.h>

// Called from loop() when a final transcript arrives from the iPhone.
typedef void (*SentenceHandler)(const String& sentence);

// Initialise the BLE transport and its optional transcript handler.
void transportInit(SentenceHandler onSentence = nullptr);

// True while an iPhone / BLE central is connected.
bool transportIsConnected();

// Send PCM16 mono audio over the transport.
//
// sampleCount is the number of int16_t samples,
// NOT the number of bytes.
void transportSendAudio(const int16_t* samples, size_t sampleCount);

// Restart advertising when needed and deliver received transcripts in loop().
void transportPoll();

// Send debug information.
//
// Since BLE is now carrying the actual application
// data, ordinary USB Serial is safe for debugging.
void transportSendLog(const String& msg);
