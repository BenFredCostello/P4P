#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>

#define I2S_WS   4
#define I2S_SCK  5
#define I2S_SD   6
#define I2S_PORT          I2S_NUM_0
#define SAMPLE_RATE       16000
#define SAMPLE_BITS       I2S_BITS_PER_SAMPLE_32BIT
#define BUFFER_SAMPLES    100

// Match Python exactly
#define DEVICE_NAME   "ESP32-Audio"
#define SERVICE_UUID  "12345678-1234-1234-1234-123456789abc"
#define CHAR_UUID     "12345678-1234-1234-1234-123456789abc"

int32_t samples[BUFFER_SAMPLES];
BLECharacteristic* pCharacteristic = nullptr;
bool deviceConnected = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Client disconnected, restarting advertising");
    BLEDevice::startAdvertising();
  }
};

void setupI2S() {
  Serial.println("[I2S] Installing driver...");
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num  = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  Serial.printf("[I2S] Driver install: %s\n", err == ESP_OK ? "OK" : "FAILED");

  err = i2s_set_pin(I2S_PORT, &pin_config);
  Serial.printf("[I2S] Pin config: %s\n", err == ESP_OK ? "OK" : "FAILED");

  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[I2S] Ready");
}

void setupBLE() {
  Serial.println("[BLE] Initializing...");
  BLEDevice::init(DEVICE_NAME);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  BLEDevice::startAdvertising();
  Serial.printf("[BLE] Advertising as '%s'\n", DEVICE_NAME);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32-Audio BLE Mic ===");
  setupI2S();
  setupBLE();
  Serial.println("[MAIN] Ready, waiting for connection...");
}

void loop() {
  if (!deviceConnected) {
    delay(100);
    return;
  }

  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read, portMAX_DELAY);

  if (err != ESP_OK) {
    Serial.printf("[I2S] Read FAILED: %d\n", err);
    return;
  }

  int samples_read = bytes_read / sizeof(int32_t);

  // Convert 32-bit I2S samples to 16-bit for Python struct.unpack('h')
  int16_t out[BUFFER_SAMPLES];
  int32_t peak = 0;
  for (int i = 0; i < samples_read; i++) {
    out[i] = (int16_t)(samples[i] >> 16);  // top 16 bits of 24-bit sample
    int32_t s = abs(out[i]);
    if (s > peak) peak = s;
  }

  Serial.printf("[MIC] Peak: %d | Sending %d bytes\n", peak, samples_read * 2);

  pCharacteristic->setValue((uint8_t*)out, samples_read * 2);
  pCharacteristic->notify();
}
