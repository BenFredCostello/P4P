// I2S capture for mic 1. See mic.h for the interface contract.
//
// XIAO ESP32S3 D-pin -> GPIO mapping used here: D0=GPIO1, D1=GPIO2, D2=GPIO3.
// Mic 1 is wired SCK=D0, WS=D1, SD=D2, L/R->GND (left channel), on I2S port 0.
//
// Mics 3+4 (not wired yet) go on I2S port 1: SCK=D3(GPIO4), WS=D8(GPIO7),
// SD=D9(GPIO8) - add a second micN_init()/micNReadChunk() pair modeled on
// this file when that hardware exists. Mic 2 shares this same I2S0 bus as
// the right channel (L/R->3V3); reading it means switching channel_format
// to I2S_CHANNEL_FMT_RIGHT_LEFT and de-interleaving in micReadChunk().

#include "mic.h"
#include "transport.h"
#include <driver/i2s.h>

#define I2S_PORT I2S_NUM_0
#define I2S_PIN_SCK 1  // D0
#define I2S_PIN_WS 2   // D1
#define I2S_PIN_SD 3   // D2

#define SAMPLE_RATE 16000
#define FRAMES_PER_CHUNK 256  // samples per channel per chunk
#define DMA_BUF_COUNT 4

// DIAGNOSTIC BUILD: mono ONLY_LEFT and ONLY_RIGHT both read as constant
// zero, so this captures both I2S slots as stereo and logs their peaks
// every ~1s. That tells us whether either slot has real signal (driver
// slot-selection issue - fix by hardcoding the right index below) or both
// are silent (wiring/power issue upstream of the I2S peripheral).

bool micInit() {
  i2s_config_t config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 puts 24-bit data in a 32-bit frame
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,   // capture both slots for diagnosis
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUF_COUNT,
      .dma_buf_len = FRAMES_PER_CHUNK,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0};

  i2s_pin_config_t pins = {.bck_io_num = I2S_PIN_SCK,
                            .ws_io_num = I2S_PIN_WS,
                            .data_out_num = I2S_PIN_NO_CHANGE,
                            .data_in_num = I2S_PIN_SD};

  if (i2s_driver_install(I2S_PORT, &config, 0, NULL) != ESP_OK) return false;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

size_t micReadChunk(int16_t *out, size_t maxSamples) {
  static int32_t raw[FRAMES_PER_CHUNK * 2];  // interleaved [slot0, slot1] per frame
  size_t frameCap = maxSamples < FRAMES_PER_CHUNK ? maxSamples : FRAMES_PER_CHUNK;

  size_t bytesRead = 0;
  // Non-blocking read so loop() can pace BLE notifications and keep the BLE
  // host task responsive. If no complete DMA data is ready yet, return 0 and
  // try again on the next loop iteration.
  esp_err_t err = i2s_read(I2S_PORT, raw, frameCap * 2 * sizeof(int32_t), &bytesRead, 0);
  if (err != ESP_OK) return 0;

  size_t framesRead = bytesRead / (2 * sizeof(int32_t));

  static unsigned long lastLog = 0;
  static int32_t peak0 = 0, peak1 = 0;
  for (size_t i = 0; i < framesRead; i++) {
    int32_t s0 = raw[i * 2];
    int32_t s1 = raw[i * 2 + 1];
    if (abs(s0) > peak0) peak0 = abs(s0);
    if (abs(s1) > peak1) peak1 = abs(s1);
  }
  unsigned long now = millis();
  if (now - lastLog >= 1000) {
    lastLog = now;
    transportSendLog("mic slot0 peak=" + String(peak0) + " slot1 peak=" + String(peak1));
    peak0 = 0;
    peak1 = 0;
  }

  for (size_t i = 0; i < framesRead; i++) {
    out[i] = (int16_t)(raw[i * 2] >> 14);  // TEMP: slot0, until the log tells us which slot is real
  }
  return framesRead;
}
