#pragma once

#include <Arduino.h>

// Function that will eventually receive transcript
// sentences sent back from the iPhone.
typedef void (*SentenceHandler)(const String& sentence);

// Initialise the transport.
//
// Currently BLE.
//
// onSentence is optional for now.
// We will use it when we add the BLE text
// characteristic for incoming transcripts.
void transportInit(SentenceHandler onSentence = nullptr);

// True while an iPhone / BLE central is connected.
bool transportIsConnected();

// Send PCM16 mono audio over the transport.
//
// sampleCount is the number of int16_t samples,
// NOT the number of bytes.
void transportSendAudio(const int16_t* samples, size_t sampleCount);

// Allow the transport to perform any required
// background processing.
//
// Nothing is required here for BLE currently,
// but keeping this function makes the transport
// abstraction cleaner.
void transportPoll();

// Send debug information.
//
// Since BLE is now carrying the actual application
// data, ordinary USB Serial is safe for debugging.
void transportSendLog(const String& msg);