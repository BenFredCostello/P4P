#pragma once
#include <Arduino.h>

// Transport abstraction over the framed protocol (protocol.h). Today this
// is backed by USB serial (transport_serial.cpp). A future BLE transport
// (transport_ble.cpp) implements the same five functions and gets swapped
// in by changing which .cpp is compiled - main .ino, mic.cpp and the
// display code only ever call these functions, never Serial directly, so
// none of them need to change when that swap happens.

typedef void (*SentenceHandler)(const String &sentence);

// Bring the transport up. `onSentence` is called once per fully-received
// PKT_TYPE_SENTENCE packet from the host.
void transportInit(unsigned long baud, SentenceHandler onSentence);

// Pump incoming bytes through the packet parser. Call every loop() iteration.
void transportPoll();

// Send one chunk of raw PCM16 audio to the host.
void transportSendAudio(const uint8_t *data, size_t len);

// Send a human-readable debug line to the host. Use this instead of
// Serial.print/println anywhere after transportInit() - unframed text
// written straight to Serial would corrupt the packet stream.
void transportSendLog(const String &msg);
