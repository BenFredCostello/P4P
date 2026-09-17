#include "mic.h"

#include <driver/i2s.h>

// ── I2S mic pins ─────────────────────────────────────────────────────────────

#define I2S_PIN_SCK 1  // D0
#define I2S_PIN_WS 2   // D1
#define I2S_PIN_SD 8   // D2
#define MIC_VCC_PIN 3  // GPIO8, used as the microphone's 3.3 V supply

#define I2S_PORT I2S_NUM_0

#define MIC_GAIN 16

// The INMP441 is being read using stereo I2S frames.
//
// Each frame contains:
//
// slot 0 -> int32_t
// slot 1 -> int32_t
//
// Previous testing showed the microphone signal
// appears in slot 1.
static int32_t rawSamples[BUFFER_SAMPLES * 2];

// ── Initialisation
// ────────────────────────────────────────────────────────────

void micInit() {
  // Power the mic before enabling its I2S clocks.
  pinMode(MIC_VCC_PIN, OUTPUT);
  digitalWrite(MIC_VCC_PIN, HIGH);
  delay(10);
  Serial.println("[MIC] VCC on D9 high");

  Serial.println("[I2S] Installing driver...");

  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),

      .sample_rate = SAMPLE_RATE,

      // INMP441 provides 24-bit audio inside
      // a 32-bit I2S slot.
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,

      // Capture both slots.
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,

      .communication_format = I2S_COMM_FORMAT_STAND_I2S,

      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

      .dma_buf_count = 4,
      .dma_buf_len = 256,

      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0};

  i2s_pin_config_t pin_config = {.bck_io_num = I2S_PIN_SCK,

                                 .ws_io_num = I2S_PIN_WS,

                                 .data_out_num = I2S_PIN_NO_CHANGE,

                                 .data_in_num = I2S_PIN_SD};

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

  Serial.printf("[I2S] Driver install: %s\n", err == ESP_OK ? "OK" : "FAILED");

  err = i2s_set_pin(I2S_PORT, &pin_config);

  Serial.printf("[I2S] Pin config: %s\n", err == ESP_OK ? "OK" : "FAILED");

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("[I2S] Ready");
}

// ── Audio capture
// ─────────────────────────────────────────────────────────────

size_t micReadChunk(int16_t* output, size_t maxSamples, int32_t* peak) {
  size_t bytesRead = 0;

  // BUFFER_SAMPLES stereo frames are requested.
  //
  // One stereo frame:
  //
  // slot0 = 4 bytes
  // slot1 = 4 bytes
  //
  // total = 8 bytes/frame
  esp_err_t err = i2s_read(I2S_PORT, rawSamples, sizeof(rawSamples), &bytesRead,
                           portMAX_DELAY);

  if (err != ESP_OK) {
    Serial.printf("[I2S] Read FAILED: %d\n", err);

    return 0;
  }

  size_t framesRead = bytesRead / (2 * sizeof(int32_t));

  if (framesRead > maxSamples) {
    framesRead = maxSamples;
  }

  int32_t localPeak = 0;

  for (size_t i = 0; i < framesRead; i++) {
    // Stereo layout:
    //
    // rawSamples[0] = slot0 frame 0
    // rawSamples[1] = slot1 frame 0
    // rawSamples[2] = slot0 frame 1
    // rawSamples[3] = slot1 frame 1
    //
    // Testing showed microphone audio is in slot 1.

    int32_t sample = rawSamples[i * 2 + 1] >> 16;

    // Increase microphone level.
    sample *= MIC_GAIN;

    // Prevent PCM16 overflow.
    sample = constrain(sample, -32768, 32767);

    output[i] = (int16_t)sample;

    int32_t magnitude = abs(sample);

    if (magnitude > localPeak) {
      localPeak = magnitude;
    }
  }

  if (peak != nullptr) {
    *peak = localPeak;
  }

  return framesRead;
}
