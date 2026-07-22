// Standalone mic 1 wiring test - no display, no serial protocol, no
// Deepgram. Just I2S capture printed straight to Serial so you can watch
// it live in the Arduino IDE's own Serial Monitor while checking wiring.
//
// XIAO ESP32S3 pins: SCK=D0 (GPIO1), WS=D1 (GPIO2), SD=D2 (GPIO3).
// Mic 1's L/R pin is tied to GND.
//
// Captures BOTH I2S slots (stereo) and prints both peaks every 200ms,
// since we don't yet know which slot the ESP32 legacy I2S driver actually
// reads mic 1's data into for this wiring.
//
// What to look for:
//  - A properly connected, quiet mic: small, slightly wobbly peak values
//    (single/low-double digits), rising into the thousands when you talk.
//  - A loose/floating SD (or SCK/WS) line: peaks slam to huge, erratic
//    values (near 2147483647) that come and go as you touch the wires.

#include <driver/i2s.h>

#define I2S_PORT I2S_NUM_0
#define I2S_PIN_SCK 1  // D0
#define I2S_PIN_WS 2   // D1
#define I2S_PIN_SD 3   // D2

#define SAMPLE_RATE 16000
#define FRAMES_PER_CHUNK 256
#define DMA_BUF_COUNT 4
#define PRINT_INTERVAL_MS 200

int32_t raw[FRAMES_PER_CHUNK * 2];  // interleaved [slot0, slot1] per frame
unsigned long lastPrint = 0;
int32_t peak0 = 0, peak1 = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== MIC 1 WIRING TEST ===");

  i2s_config_t config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // capture both slots
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

  if (i2s_driver_install(I2S_PORT, &config, 0, NULL) != ESP_OK) {
    Serial.println("i2s_driver_install FAILED");
    while (true) delay(1000);
  }
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("i2s_set_pin FAILED");
    while (true) delay(1000);
  }
  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("I2S ready. Watching for mic signal...");
}

void loop() {
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(50));
  if (err == ESP_OK) {
    size_t framesRead = bytesRead / (2 * sizeof(int32_t));
    for (size_t i = 0; i < framesRead; i++) {
      int32_t s0 = raw[i * 2];
      int32_t s1 = raw[i * 2 + 1];
      if (abs(s0) > peak0) peak0 = abs(s0);
      if (abs(s1) > peak1) peak1 = abs(s1);
    }
  }

  unsigned long now = millis();
  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;
    Serial.print("slot0 peak=");
    Serial.print(peak0);
    Serial.print("  slot1 peak=");
    Serial.println(peak1);
    peak0 = 0;
    peak1 = 0;
  }
}
