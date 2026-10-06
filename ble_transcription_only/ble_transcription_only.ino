/*
  STANDALONE BLE TRANSCRIPTION TEST - XIAO ESP32S3

  Purpose:
    Stream microphone audio over BLE to the existing iPhone app (or laptop bridge)
    and print returned transcription text to Serial Monitor.

  No OLED.
  No latency measurement.
  No trial arming.
  No threshold/speech detection.
  Just connect the phone/app and speak.

  Serial Monitor: 115200 baud

  Audio:
    16 kHz mono PCM16
    90 samples = 180 bytes per BLE notification

  BLE:
    Device name: ESP32-Audio
    Service UUID and characteristic UUIDs match the existing project/app.
*/

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <driver/i2s.h>
#include <string.h>

// -------------------- Microphone --------------------

constexpr int MIC_SCK_PIN = 1;
constexpr int MIC_WS_PIN = 2;
constexpr int MIC_SD_PIN = 8;       // XIAO D9
constexpr int MIC_POWER_PIN = 3;    // XIAO D2
constexpr bool USE_GPIO_MIC_POWER = true;

constexpr i2s_port_t MIC_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t BLOCK_SAMPLES = 90;
constexpr int32_t MIC_GAIN = 16;
constexpr size_t MAX_AUDIO_BYTES = 180;

int32_t rawSamples[BLOCK_SAMPLES * 2];
int16_t pcmSamples[BLOCK_SAMPLES];

// -------------------- BLE --------------------

constexpr char DEVICE_NAME[] = "ESP32-Audio";

constexpr char SERVICE_UUID[] =
    "12345678-1234-1234-1234-123456789abc";

constexpr char AUDIO_UUID[] =
    "12345678-1234-1234-1234-123456789abc";

constexpr char TEXT_UUID[] =
    "87654321-4321-4321-4321-cba987654321";

BLEServer* bleServer = nullptr;
BLECharacteristic* audioCharacteristic = nullptr;

volatile bool connected = false;

// -------------------- Returned transcription --------------------

constexpr size_t MAX_TEXT_BYTES = 256;

portMUX_TYPE textMux = portMUX_INITIALIZER_UNLOCKED;
char pendingText[MAX_TEXT_BYTES + 1] = {};
volatile bool newTextAvailable = false;

// -------------------- BLE callbacks --------------------

class LinkCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    connected = true;
  }

  void onDisconnect(BLEServer*) override {
    connected = false;
  }
};

class TextCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const String text = characteristic->getValue();

    portENTER_CRITICAL(&textMux);

    const size_t length =
        text.length() < MAX_TEXT_BYTES ? text.length() : MAX_TEXT_BYTES;

    memcpy(pendingText, text.c_str(), length);
    pendingText[length] = '\0';
    newTextAvailable = true;

    portEXIT_CRITICAL(&textMux);
  }
};

// -------------------- Microphone setup --------------------

bool initMicrophone() {
  if (USE_GPIO_MIC_POWER) {
    pinMode(MIC_POWER_PIN, OUTPUT);
    digitalWrite(MIC_POWER_PIN, HIGH);
    delay(10);
  }

  i2s_config_t config = {};

  config.mode =
      static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);

  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;

  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;

  i2s_pin_config_t pins = {};

  pins.bck_io_num = MIC_SCK_PIN;
  pins.ws_io_num = MIC_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD_PIN;

  const esp_err_t installed =
      i2s_driver_install(MIC_PORT, &config, 0, nullptr);

  if (installed != ESP_OK) {
    Serial.printf("[ERROR] I2S install failed: %d\n", installed);
    return false;
  }

  const esp_err_t assigned = i2s_set_pin(MIC_PORT, &pins);

  if (assigned != ESP_OK) {
    Serial.printf("[ERROR] I2S pin setup failed: %d\n", assigned);
    return false;
  }

  i2s_zero_dma_buffer(MIC_PORT);

  return true;
}

// -------------------- BLE setup --------------------

void initBle() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new LinkCallbacks());
  bleServer->advertiseOnDisconnect(true);

  BLEService* service =
      bleServer->createService(SERVICE_UUID);

  audioCharacteristic =
      service->createCharacteristic(
          AUDIO_UUID,
          BLECharacteristic::PROPERTY_NOTIFY);

#if defined(CONFIG_BLUEDROID_ENABLED)
  audioCharacteristic->addDescriptor(new BLE2902());
#endif

  BLECharacteristic* textCharacteristic =
      service->createCharacteristic(
          TEXT_UUID,
          BLECharacteristic::PROPERTY_WRITE |
              BLECharacteristic::PROPERTY_WRITE_NR);

  textCharacteristic->setCallbacks(new TextCallbacks());

  service->start();

  BLEAdvertising* advertising =
      BLEDevice::getAdvertising();

  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();
}

// -------------------- Audio streaming --------------------

void streamAudio(const int16_t* samples, size_t sampleCount) {
  if (!connected) return;

  const uint16_t mtu =
      bleServer->getPeerMTU(bleServer->getConnId());

  size_t payload = mtu > 3 ? mtu - 3 : 20;

  if (payload > MAX_AUDIO_BYTES)
    payload = MAX_AUDIO_BYTES;

  // PCM16 must stay aligned to 2-byte samples.
  payload &= ~static_cast<size_t>(1);

  if (payload < 2) return;

  const uint8_t* bytes =
      reinterpret_cast<const uint8_t*>(samples);

  size_t remaining =
      sampleCount * sizeof(int16_t);

  while (remaining && connected) {
    const size_t count =
        remaining < payload ? remaining : payload;

    audioCharacteristic->setValue(bytes, count);
    audioCharacteristic->notify();

    bytes += count;
    remaining -= count;
  }
}

// -------------------- Setup --------------------

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("Standalone BLE transcription test");
  Serial.println("No OLED and no latency measurement.");

  if (!initMicrophone()) {
    Serial.println("[FATAL] Microphone setup failed.");
    return;
  }

  initBle();

  Serial.println("[BLE] Advertising as ESP32-Audio.");
  Serial.println("Connect the iPhone app, then speak normally.");
  Serial.println("Returned transcription will appear below.");
}

// -------------------- Main loop --------------------

void loop() {
  // Print any returned transcription outside the BLE callback.
  if (newTextAvailable) {
    char textCopy[MAX_TEXT_BYTES + 1];

    portENTER_CRITICAL(&textMux);

    strncpy(textCopy, pendingText, MAX_TEXT_BYTES);
    textCopy[MAX_TEXT_BYTES] = '\0';
    newTextAvailable = false;

    portEXIT_CRITICAL(&textMux);

    Serial.print("[TRANSCRIPT] ");
    Serial.println(textCopy);
  }

  if (!connected) {
    delay(10);
    return;
  }

  size_t bytesRead = 0;

  const esp_err_t error =
      i2s_read(
          MIC_PORT,
          rawSamples,
          sizeof(rawSamples),
          &bytesRead,
          pdMS_TO_TICKS(20));

  if (error != ESP_OK || bytesRead == 0 || bytesRead % 8 != 0) {
    return;
  }

  const size_t frames =
      bytesRead / (2 * sizeof(int32_t));

  for (size_t i = 0; i < frames; ++i) {
    int32_t sample =
        (rawSamples[i * 2 + 1] >> 16) * MIC_GAIN;

    sample = constrain(sample, -32768, 32767);

    pcmSamples[i] =
        static_cast<int16_t>(sample);
  }

  streamAudio(pcmSamples, frames);
}
