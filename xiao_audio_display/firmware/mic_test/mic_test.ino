#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <driver/i2s.h>

// ── I2S mic pins ─────────────────────────────────────────────────────────────
#define I2S_PIN_SCK 1  // D0
#define I2S_PIN_WS  2  // D1
#define I2S_PIN_SD  3  // D2

#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE 16000

// Number of mono samples sent per read.
// 100 samples @ 16 kHz = 6.25 ms audio.
#define BUFFER_SAMPLES 90

#define MIC_GAIN 16

#define MAX_BLE_AUDIO_PAYLOAD 180

// ── BLE config ────────────────────────────────────────────────────────────────
#define DEVICE_NAME "ESP32-Audio"
#define SERVICE_UUID "12345678-1234-1234-1234-123456789abc"
#define CHAR_UUID "12345678-1234-1234-1234-123456789abc"

// Stereo I2S:
// each frame has slot0 + slot1
int32_t rawSamples[BUFFER_SAMPLES * 2];

BLECharacteristic* pCharacteristic = nullptr;
BLEServer* pServer = nullptr;

bool deviceConnected = false;

// ── BLE callbacks ─────────────────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
  }

  void onDisconnect(BLEServer* server) override {
    deviceConnected = false;

    Serial.println(
        "[BLE] Client disconnected, restarting advertising"
    );

    BLEDevice::startAdvertising();
  }
};

// ── I2S setup ─────────────────────────────────────────────────────────────────
void setupI2S() {
  Serial.println("[I2S] Installing driver...");

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),

    .sample_rate = SAMPLE_RATE,

    // INMP441 gives 24-bit audio inside a 32-bit I2S slot
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,

    // IMPORTANT:
    // capture BOTH slots, because previous testing showed
    // the microphone data appears in slot 1
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,

    .communication_format = I2S_COMM_FORMAT_STAND_I2S,

    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 4,
    .dma_buf_len = 256,

    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_PIN_SCK,
    .ws_io_num = I2S_PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_PIN_SD
  };

  esp_err_t err =
      i2s_driver_install(
          I2S_PORT,
          &i2s_config,
          0,
          NULL
      );

  Serial.printf(
      "[I2S] Driver install: %s\n",
      err == ESP_OK ? "OK" : "FAILED"
  );

  err = i2s_set_pin(
      I2S_PORT,
      &pin_config
  );

  Serial.printf(
      "[I2S] Pin config: %s\n",
      err == ESP_OK ? "OK" : "FAILED"
  );

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("[I2S] Ready");
}

// ── BLE setup ─────────────────────────────────────────────────────────────────
void setupBLE() {
  Serial.println("[BLE] Initializing...");

  BLEDevice::init(DEVICE_NAME);

  BLEDevice::setMTU(185);

  pServer = BLEDevice::createServer();

  pServer->setCallbacks(
      new ServerCallbacks()
  );

  BLEService* pService =
      pServer->createService(
          SERVICE_UUID
      );

  pCharacteristic =
      pService->createCharacteristic(
          CHAR_UUID,
          BLECharacteristic::PROPERTY_NOTIFY
      );

  pCharacteristic->addDescriptor(
      new BLE2902()
  );

  pService->start();

  BLEAdvertising* advertising =
      BLEDevice::getAdvertising();

  advertising->addServiceUUID(
      SERVICE_UUID
  );

  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.printf(
      "[BLE] Advertising as '%s'\n",
      DEVICE_NAME
  );
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("=== ESP32-Audio BLE Mic Test ===");

  setupI2S();
  setupBLE();

  Serial.println(
      "[MAIN] Waiting for laptop connection..."
  );
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {

  if (!deviceConnected) {
    delay(50);
    return;
  }

  size_t bytesRead = 0;

  // We want BUFFER_SAMPLES stereo frames.
  // Each frame =
  // 32-bit slot0 + 32-bit slot1 = 8 bytes.
  esp_err_t err =
      i2s_read(
          I2S_PORT,
          rawSamples,
          sizeof(rawSamples),
          &bytesRead,
          portMAX_DELAY
      );

  if (err != ESP_OK) {
    Serial.printf(
        "[I2S] Read FAILED: %d\n",
        err
    );

    return;
  }

  // 8 bytes per stereo frame
  size_t framesRead =
      bytesRead /
      (2 * sizeof(int32_t));

  // Output mono PCM16 for Python / Deepgram
  int16_t out[BUFFER_SAMPLES];

  int32_t peak = 0;

  for (size_t i = 0; i < framesRead; i++) {

    // Stereo data layout:
    //
    // rawSamples[0] = slot0 frame 0
    // rawSamples[1] = slot1 frame 0
    // rawSamples[2] = slot0 frame 1
    // rawSamples[3] = slot1 frame 1
    //
    // Previous testing showed mic is in slot1.

    int32_t sample =
        rawSamples[i * 2 + 1] >> 16;

    // Boost mic level
    sample *= MIC_GAIN;

    // Prevent overflow
    sample = constrain(
        sample,
        -32768,
        32767
    );

    out[i] = (int16_t)sample;

    int32_t magnitude = abs(sample);

    if (magnitude > peak) {
      peak = magnitude;
    }
  }

  // Don't print every packet because Serial printing itself
  // can introduce extra latency.
    uint16_t mtu =
      pServer->getPeerMTU(
          pServer->getConnId()
      );

  // Don't print every packet because Serial printing itself
  // can introduce extra latency.
  static unsigned long lastPrint = 0;

  if (millis() - lastPrint > 500) {
    lastPrint = millis();

    Serial.printf(
        "[MIC] Peak: %ld | Frames: %d | MTU: %d\n",
        peak,
        (int)framesRead,
        mtu
    );
  }

  size_t payload =
      mtu > 3
          ? mtu - 3
          : 20;

  if (payload > MAX_BLE_AUDIO_PAYLOAD) {
    payload =
        MAX_BLE_AUDIO_PAYLOAD;
  }

  // PCM16 = 2 bytes/sample
  payload &= ~((size_t)1);

  // ── Send audio ──────────────────────────────────────────────────────────────
  const uint8_t* audio =
      reinterpret_cast<const uint8_t*>(out);

  size_t bytesRemaining =
      framesRead * sizeof(int16_t);

  while (
      bytesRemaining > 0 &&
      deviceConnected
  ) {

    size_t packetBytes =
        bytesRemaining < payload
            ? bytesRemaining
            : payload;

    pCharacteristic->setValue(
        audio,
        packetBytes
    );

    pCharacteristic->notify();

    audio += packetBytes;
    bytesRemaining -= packetBytes;

    // Pace notifications at approximately the real audio rate.
    //
    // 2 bytes = 1 PCM16 sample.
  }
}