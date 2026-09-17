#include <Arduino.h>

#include "mic.h"
#include "transport.h"

// One chunk of PCM16 mono audio.
int16_t audioBuffer[BUFFER_SAMPLES];

void onSentence(const String& sentence) {
  Serial.printf("[TRANSCRIPT] %s\n", sentence.c_str());
}

void setup() {
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("=== ESP32-Audio ===");

  // Set up microphone / I2S.
  micInit();

  // Set up BLE.
  transportInit(onSentence);

  Serial.println("[MAIN] Waiting for connection...");
}

void loop() {
  // Currently does nothing for BLE,
  // but kept as part of the transport interface.
  transportPoll();

  // Don't capture/send audio until a BLE client is connected.
  if (!transportIsConnected()) {
    delay(50);
    return;
  }

  int32_t peak = 0;

  size_t samplesRead =
      micReadChunk(
          audioBuffer,
          BUFFER_SAMPLES,
          &peak
      );

  if (samplesRead == 0) {
    return;
  }

  // Don't spam Serial because excessive logging
  // can affect real-time streaming.
  static unsigned long lastPrint = 0;

  if (millis() - lastPrint > 500) {
    lastPrint = millis();

    Serial.printf(
        "[MIC] Peak: %ld | Samples: %d\n",
        peak,
        (int)samplesRead
    );
  }

  // Send PCM16 samples to the iPhone over BLE.
  transportSendAudio(
      audioBuffer,
      samplesRead
  );
}
