#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "protocol.h"
#include "transport.h"
#include "mic.h"

// Set this to 1 for the first stability test. The ESP sends 20 dummy bytes
// every 100 ms and does not stream microphone audio. Once BLE and the OLED
// remain stable, change it to 0 for real microphone audio.
#define BLE_DUMMY_TEST 1

// Panel is 72x40 but the SSD1306 controller has 128x64 RAM.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
#define OLED_SDA_PIN 5  // XIAO D4
#define OLED_SCL_PIN 6  // XIAO D5

#define X_OFF 28
#define Y_OFF 24
#define VIS_W 72
#define VIS_H 40

#define CHARS_PER_LINE 12
#define MAX_LINES 4
#define LINE_HEIGHT 8
#define MIN_SPLIT 2
#define MAX_DISPLAY_SOURCE_CHARS 64

#define CHECK_INTERVAL_MS 500
#define SERIAL_BAUD 921600
#define AUDIO_CHUNK_SAMPLES 256

#define DEVICE_NAME "ESP32-Audio"
#define SERVICE_UUID "12345678-1234-1234-1234-123456789abc"
#define AUDIO_CHAR_UUID "12345678-1234-1234-1234-123456789abc"
#define TEXT_CHAR_UUID "87654321-4321-4321-4321-cba987654321"

// iPhones normally negotiate an ATT MTU of 185, giving 182 payload bytes.
// Use 180 as the maximum and always reduce it to the actual negotiated MTU.
#define MAX_BLE_AUDIO_PAYLOAD 180
#define MIN_STREAMING_MTU 100
#define STREAM_START_DELAY_MS 1000
#define DUMMY_INTERVAL_MS 100

// About 250 ms of PCM16 mono audio at 16 kHz.
#define AUDIO_RING_BYTES 8192

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

BLEServer *bleServer = nullptr;
BLECharacteristic *audioCharacteristic = nullptr;
BLECharacteristic *textCharacteristic = nullptr;

volatile bool bluetoothConnected = false;
volatile bool bleConnectionChanged = false;
volatile bool notifyCallActive = false;
volatile bool notifyEnqueueAccepted = false;
volatile bool notifyPending = false;
volatile bool notifyCompletionSeen = false;
volatile bool notifyCompletionSucceeded = false;
volatile uint32_t notifySuccessCount = 0;
volatile uint32_t notifyFailureCount = 0;
volatile uint32_t lastNotifyErrorCode = 0;
size_t pendingPayloadLength = 0;
bool pendingPayloadUsesAudioRing = false;

unsigned long bleConnectedAt = 0;
unsigned long lastBleStats = 0;
unsigned long lastDummySend = 0;
uint32_t nextAudioSendUs = 0;

bool displayReady = false;
bool startupDisplayFlashShown = false;
String currentSentence = "";
unsigned long lastCheck = 0;

uint8_t audioRing[AUDIO_RING_BYTES];
size_t audioRingRead = 0;
size_t audioRingWrite = 0;
size_t audioRingUsed = 0;
uint32_t droppedAudioBytes = 0;

struct DisplayMessage {
  char text[193];
};

QueueHandle_t displayQueue = nullptr;

// -------------------------------------------------------------------------
// OLED code from the last working sketch
// -------------------------------------------------------------------------

bool displayConnected() {
  Wire.beginTransmission(OLED_ADDR);
  return (Wire.endTransmission() == 0);
}

void renderSentence(const String &text) {
  // For long live transcripts, show the newest words rather than leaving the
  // OLED stuck on the beginning of the sentence.
  String visibleText = text;
  if (visibleText.length() > MAX_DISPLAY_SOURCE_CHARS) {
    visibleText = visibleText.substring(
        visibleText.length() - MAX_DISPLAY_SOURCE_CHARS);
    int firstSpace = visibleText.indexOf(' ');
    if (firstSpace >= 0) visibleText.remove(0, firstSpace + 1);
  }

  String lines[MAX_LINES];
  int line = 0;
  unsigned int pos = 0;

  while (pos < visibleText.length() && line < MAX_LINES) {
    while (pos < visibleText.length() && visibleText[pos] == ' ') pos++;
    if (pos >= visibleText.length()) break;

    int wordEnd = visibleText.indexOf(' ', pos);
    if (wordEnd == -1) wordEnd = visibleText.length();
    String word = visibleText.substring(pos, wordEnd);

    bool lineHasText = lines[line].length() > 0;
    int sep = lineHasText ? 1 : 0;
    int space = CHARS_PER_LINE - lines[line].length();

    if ((int)word.length() + sep <= space) {
      if (lineHasText) lines[line] += ' ';
      lines[line] += word;
      pos = wordEnd;
    } else {
      int room = space - sep;
      if (room >= MIN_SPLIT + 1) {
        int take = room - 1;
        if (lineHasText) lines[line] += ' ';
        lines[line] += word.substring(0, take) + '-';
        pos += take;
      }
      line++;
    }
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  String debugSummary = "drew: ";
  for (int i = 0; i < MAX_LINES; i++) {
    display.setCursor(X_OFF, Y_OFF + i * LINE_HEIGHT);
    display.print(lines[i]);
    debugSummary += "[" + lines[i] + "]";
  }
  display.display();
  transportSendLog(debugSummary);
}

void drawSentence(const String &text) {
  currentSentence = text;
  if (displayReady) {
    renderSentence(text);
  } else {
    transportSendLog("(display offline - sentence stored, will draw on reconnect)");
  }
}

bool initDisplay() {
  if (!displayConnected()) return false;
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) return false;

  if (!startupDisplayFlashShown) {
    display.clearDisplay();
    display.fillRect(X_OFF, Y_OFF, VIS_W, VIS_H, SSD1306_WHITE);
    display.display();
    delay(300);
    startupDisplayFlashShown = true;
  }

  displayReady = true;
  transportSendLog("Display connected!");

  if (currentSentence.length() > 0) renderSentence(currentSentence);
  else {
    display.clearDisplay();
    display.display();
  }

  return true;
}

void onSentenceReceived(const String &sentence) {
  drawSentence(sentence);
}

// BLE callbacks must not use Wire or draw to the OLED. They place text in a
// queue, and loop() performs the actual display update.
void queueDisplayText(const String &text) {
  if (displayQueue == nullptr) return;

  DisplayMessage message = {};
  size_t count = text.length();
  if (count >= sizeof(message.text)) count = sizeof(message.text) - 1;
  memcpy(message.text, text.c_str(), count);
  message.text[count] = '\0';
  xQueueOverwrite(displayQueue, &message);
}

void processDisplayQueue() {
  if (displayQueue == nullptr) return;

  DisplayMessage message;
  if (xQueueReceive(displayQueue, &message, 0) == pdTRUE) {
    drawSentence(String(message.text));
  }
}

// -------------------------------------------------------------------------
// BLE callbacks and setup
// -------------------------------------------------------------------------

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bluetoothConnected = true;
    bleConnectionChanged = true;
    bleConnectedAt = millis();
  }

  void onDisconnect(BLEServer *server) override {
    bluetoothConnected = false;
    bleConnectionChanged = true;
    notifyPending = false;
    notifyCompletionSeen = false;
  }
};

class AudioCallbacks : public BLECharacteristicCallbacks {
  void onStatus(
      BLECharacteristic *characteristic,
      BLECharacteristicCallbacks::Status status,
      uint32_t code) override {
    lastNotifyErrorCode = code;

    // Arduino-ESP32's NimBLE wrapper calls onStatus once inside notify() to
    // report whether the packet was queued, then again from the NimBLE host
    // task when transmission completes. Keep those two results separate.
    if (notifyCallActive) {
      notifyEnqueueAccepted =
          status == BLECharacteristicCallbacks::Status::SUCCESS_NOTIFY;
      // Any later callback belongs to the actual NimBLE transmit event,
      // even if it arrives before notify() has fully returned.
      notifyCallActive = false;
      return;
    }

    if (!notifyPending) return;

    notifyCompletionSucceeded =
        status == BLECharacteristicCallbacks::Status::SUCCESS_NOTIFY;
    notifyCompletionSeen = true;
  }
};

class TextCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String sentence = characteristic->getValue();
    sentence.trim();
    if (sentence.length() > 0) {
      queueDisplayText(sentence);
    }
  }
};

void setupBluetooth() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  bleServer->advertiseOnDisconnect(true);

  BLEService *service = bleServer->createService(SERVICE_UUID);

  audioCharacteristic = service->createCharacteristic(
      AUDIO_CHAR_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  audioCharacteristic->setCallbacks(new AudioCallbacks());

  textCharacteristic = service->createCharacteristic(
      TEXT_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  textCharacteristic->setCallbacks(new TextCallbacks());

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  transportSendLog("BLE advertising as ESP32-Audio");
}

// -------------------------------------------------------------------------
// Non-blocking audio queue
// -------------------------------------------------------------------------

void clearAudioRing() {
  audioRingRead = 0;
  audioRingWrite = 0;
  audioRingUsed = 0;
}

void queueAudio(const uint8_t *data, size_t length) {
  if (length > AUDIO_RING_BYTES - audioRingUsed) {
    droppedAudioBytes += length;
    return;
  }

  for (size_t i = 0; i < length; i++) {
    audioRing[audioRingWrite] = data[i];
    audioRingWrite = (audioRingWrite + 1) % AUDIO_RING_BYTES;
  }
  audioRingUsed += length;
}

void copyAudioFromRing(uint8_t *destination, size_t length) {
  for (size_t i = 0; i < length; i++) {
    destination[i] = audioRing[(audioRingRead + i) % AUDIO_RING_BYTES];
  }
}

void removeAudioFromRing(size_t length) {
  if (length > audioRingUsed) length = audioRingUsed;
  audioRingRead = (audioRingRead + length) % AUDIO_RING_BYTES;
  audioRingUsed -= length;
}

size_t negotiatedAudioPayload() {
  if (bleServer == nullptr || !bluetoothConnected) return 0;

  uint16_t mtu = bleServer->getPeerMTU(bleServer->getConnId());
  if (mtu < MIN_STREAMING_MTU) return 0;

  size_t payload = mtu - 3;
  if (payload > MAX_BLE_AUDIO_PAYLOAD) payload = MAX_BLE_AUDIO_PAYLOAD;

  // PCM16 packets must contain a whole number of samples.
  payload &= ~((size_t)1);
  return payload;
}

bool sendNotification(
    const uint8_t *data,
    size_t length,
    bool usesAudioRing) {
  if (!bluetoothConnected || audioCharacteristic == nullptr || length == 0) {
    return false;
  }
  if (notifyPending) return false;

  notifyPending = true;
  notifyCompletionSeen = false;
  notifyCompletionSucceeded = false;
  notifyEnqueueAccepted = false;
  pendingPayloadLength = length;
  pendingPayloadUsesAudioRing = usesAudioRing;

  audioCharacteristic->setValue(data, length);
  notifyCallActive = true;
  audioCharacteristic->notify();
  notifyCallActive = false;

  if (!notifyEnqueueAccepted) {
    notifyPending = false;
    notifyFailureCount++;
    return false;
  }

  return true;
}

void processNotificationCompletion() {
  if (!notifyPending || !notifyCompletionSeen) return;

  bool succeeded = notifyCompletionSucceeded;
  bool removeFromRing = pendingPayloadUsesAudioRing;
  size_t completedLength = pendingPayloadLength;

  notifyCompletionSeen = false;
  notifyPending = false;

  if (succeeded) {
    if (removeFromRing) removeAudioFromRing(completedLength);
    notifySuccessCount++;
  } else {
    // Leave audio in the ring so the same bytes can be retried.
    notifyFailureCount++;
    nextAudioSendUs = micros() + 20000;
  }
}

void serviceDummyBluetooth() {
  if (!bluetoothConnected) return;
  if (millis() - bleConnectedAt < STREAM_START_DELAY_MS) return;
  processNotificationCompletion();
  if (notifyPending) return;
  if (millis() - lastDummySend < DUMMY_INTERVAL_MS) return;

  lastDummySend = millis();
  uint8_t dummy[20] = {};
  sendNotification(dummy, sizeof(dummy), false);
}

void captureMicrophoneAudio() {
  static int16_t audioBuffer[AUDIO_CHUNK_SAMPLES];
  size_t samplesRead = micReadChunk(audioBuffer, AUDIO_CHUNK_SAMPLES);

  if (samplesRead > 0) {
    queueAudio(
        reinterpret_cast<const uint8_t *>(audioBuffer),
        samplesRead * sizeof(int16_t));
  }
}

void serviceBluetoothAudio() {
  if (!bluetoothConnected) return;
  if (millis() - bleConnectedAt < STREAM_START_DELAY_MS) return;

  processNotificationCompletion();
  if (notifyPending) return;

  size_t payload = negotiatedAudioPayload();
  if (payload == 0 || audioRingUsed < payload) return;

  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextAudioSendUs) < 0) return;

  uint8_t packet[MAX_BLE_AUDIO_PAYLOAD];
  copyAudioFromRing(packet, payload);

  if (sendNotification(packet, payload, true)) {
    // PCM16 mono at 16 kHz is 32,000 bytes per second. Pacing each packet
    // at its real audio duration avoids filling the BLE transmit queue.
    uint32_t intervalUs = (uint32_t)(((uint64_t)payload * 1000000ULL) / 32000ULL);
    nextAudioSendUs = nowUs + intervalUs;
  } else {
    // Leave the audio in the ring and give BLE time to recover.
    nextAudioSendUs = nowUs + 20000;
  }
}

void reportBleStatus() {
  if (bleConnectionChanged) {
    bleConnectionChanged = false;

    if (bluetoothConnected) {
      clearAudioRing();
      nextAudioSendUs = micros();
      queueDisplayText("Bluetooth connected");
      transportSendLog("BLE connected");
    } else {
      clearAudioRing();
      queueDisplayText("Bluetooth disconnected");
      transportSendLog("BLE disconnected; advertising restarted");
    }
  }

  if (millis() - lastBleStats >= 1000) {
    lastBleStats = millis();

    if (bluetoothConnected) {
      uint16_t mtu = bleServer->getPeerMTU(bleServer->getConnId());
      transportSendLog(
          "BLE mtu=" + String(mtu) +
          " ok=" + String(notifySuccessCount) +
          " fail=" + String(notifyFailureCount) +
          " err=" + String(lastNotifyErrorCode) +
          " queued=" + String(audioRingUsed) +
          " dropped=" + String(droppedAudioBytes));
    }
  }
}

void setup() {
  transportInit(SERIAL_BAUD, onSentenceReceived);
  delay(1500);
  transportSendLog("=== OLED + MIC + BLE STARTING ===");

  displayQueue = xQueueCreate(1, sizeof(DisplayMessage));

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  currentSentence = "Testing the microphone sentence display";
  initDisplay();

  if (!micInit()) {
    transportSendLog("Mic init FAILED");
  } else {
    transportSendLog("Mic ready (I2S0, 16kHz mono)");
  }

  setupBluetooth();
}

void loop() {
  unsigned long now = millis();

  if (now - lastCheck >= CHECK_INTERVAL_MS) {
    lastCheck = now;
    bool connected = displayConnected();

    if (!displayReady && connected) {
      initDisplay();
    } else if (!displayReady && !connected) {
      transportSendLog("Display not found!");
    } else if (displayReady && !connected) {
      transportSendLog("Display connection LOST - waiting for it to come back...");
      displayReady = false;
    }
  }

  transportPoll();
  reportBleStatus();
  processDisplayQueue();

#if BLE_DUMMY_TEST
  serviceDummyBluetooth();
#else
  captureMicrophoneAudio();
  serviceBluetoothAudio();
#endif

  // Let the BLE host task and other ESP system tasks run.
  delay(1);
}
