#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <cstring>

#include "transport.h"

// ── BLE configuration ─────────────────────────────────────────────────────────

#define DEVICE_NAME "ESP32-Audio"

#define SERVICE_UUID \
    "12345678-1234-1234-1234-123456789abc"

#define AUDIO_CHAR_UUID \
    "12345678-1234-1234-1234-123456789abc"

#define TEXT_CHAR_UUID \
    "87654321-4321-4321-4321-cba987654321"

#define MAX_BLE_AUDIO_PAYLOAD 180
#define MAX_BLE_TEXT_PAYLOAD 180

// Give the BLE stack a little time to clean up
// before advertising again after a disconnect.
#define ADVERTISING_RESTART_DELAY_MS 500


// ── BLE state ─────────────────────────────────────────────────────────────────

static BLECharacteristic* audioCharacteristic =
    nullptr;

static BLECharacteristic* textCharacteristic =
    nullptr;

static BLEServer* server =
    nullptr;

static volatile bool deviceConnected =
    false;

static volatile bool restartAdvertisingPending =
    false;

static volatile unsigned long disconnectedAt =
    0;

// Called whenever transcript text is received from the iPhone.
static SentenceHandler sentenceHandler =
    nullptr;

// Keep the BLE write callback short. The latest sentence is handed to loop().
static portMUX_TYPE sentenceMux = portMUX_INITIALIZER_UNLOCKED;
static char pendingSentence[MAX_BLE_TEXT_PAYLOAD + 1] = {};
static bool sentenceReady = false;


// ── BLE server callbacks ──────────────────────────────────────────────────────

class ServerCallbacks :
    public BLEServerCallbacks {

  void onConnect(
      BLEServer* server
  ) override {

    deviceConnected = true;

    Serial.println(
        "[BLE] Client connected"
    );
  }

  void onDisconnect(
      BLEServer* server
  ) override {

    deviceConnected = false;

    portENTER_CRITICAL(&sentenceMux);
    sentenceReady = false;
    portEXIT_CRITICAL(&sentenceMux);

    disconnectedAt =
        millis();

    restartAdvertisingPending =
        true;

    Serial.println(
        "[BLE] Client disconnected"
    );

    // Do not restart advertising directly here.
    // transportPoll() handles that after a short delay.
  }
};


// ── Incoming transcript callback ──────────────────────────────────────────────

class TextCallbacks :
    public BLECharacteristicCallbacks {

  void onWrite(
      BLECharacteristic* characteristic
  ) override {

    if (sentenceHandler == nullptr) {
      return;
    }

    String text = characteristic->getValue();

    if (text.length() == 0) {
      return;
    }

    size_t length = text.length();
    if (length > MAX_BLE_TEXT_PAYLOAD) {
      length = MAX_BLE_TEXT_PAYLOAD;
    }

    portENTER_CRITICAL(&sentenceMux);
    memcpy(pendingSentence, text.c_str(), length);
    pendingSentence[length] = '\0';
    sentenceReady = true;
    portEXIT_CRITICAL(&sentenceMux);
  }
};


// ── Transport initialisation ──────────────────────────────────────────────────

void transportInit(
    SentenceHandler onSentence
) {
  sentenceHandler =
      onSentence;

  Serial.println(
      "[BLE] Initializing..."
  );

  BLEDevice::init(
      DEVICE_NAME
  );

  // Request MTU 185.
  BLEDevice::setMTU(185);

  server =
      BLEDevice::createServer();

  server->setCallbacks(
      new ServerCallbacks()
  );

  BLEService* service =
      server->createService(
          SERVICE_UUID
      );


  // ── Audio characteristic ───────────────────────────────────────────────────

  // ESP32 -> iPhone
  audioCharacteristic =
      service->createCharacteristic(
          AUDIO_CHAR_UUID,
          BLECharacteristic::PROPERTY_NOTIFY
      );

  audioCharacteristic->addDescriptor(
      new BLE2902()
  );


  // ── Transcript characteristic ──────────────────────────────────────────────

  // iPhone -> ESP32
  textCharacteristic =
      service->createCharacteristic(
          TEXT_CHAR_UUID,
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR
      );

  textCharacteristic->setCallbacks(
      new TextCallbacks()
  );


  // ── Start service / advertising ────────────────────────────────────────────

  service->start();

  BLEAdvertising* advertising =
      BLEDevice::getAdvertising();

  advertising->addServiceUUID(
      SERVICE_UUID
  );

  advertising->setScanResponse(
      true
  );

  BLEDevice::startAdvertising();

  Serial.printf(
      "[BLE] Advertising as '%s'\n",
      DEVICE_NAME
  );
}


// ── Connection state ──────────────────────────────────────────────────────────

bool transportIsConnected() {
  return deviceConnected;
}


// ── Audio transmission ────────────────────────────────────────────────────────

void transportSendAudio(
    const int16_t* samples,
    size_t sampleCount
) {
  if (!deviceConnected) {
    return;
  }

  uint16_t mtu =
      server->getPeerMTU(
          server->getConnId()
      );

  // ATT notification payload is MTU - 3.
  size_t payload =
      mtu > 3
          ? mtu - 3
          : 20;

  // Keep our packets at a maximum of 180 bytes.
  if (
      payload >
      MAX_BLE_AUDIO_PAYLOAD
  ) {
    payload =
        MAX_BLE_AUDIO_PAYLOAD;
  }

  // PCM16 = 2 bytes per sample,
  // so keep packet length even.
  payload &=
      ~((size_t)1);

  const uint8_t* audio =
      reinterpret_cast<
          const uint8_t*
      >(samples);

  size_t bytesRemaining =
      sampleCount *
      sizeof(int16_t);

  while (
      bytesRemaining > 0 &&
      deviceConnected
  ) {

    size_t packetBytes =
        bytesRemaining < payload
            ? bytesRemaining
            : payload;

    audioCharacteristic->setValue(
        audio,
        packetBytes
    );

    audioCharacteristic->notify();

    audio += packetBytes;

    bytesRemaining -=
        packetBytes;

    // No artificial delay.
    //
    // i2s_read() already naturally paces
    // the stream at the microphone sample rate.
  }
}


// ── Transport housekeeping ────────────────────────────────────────────────────

void transportPoll() {

  // If a BLE client disconnected,
  // wait briefly before restarting advertising.
  if (
      restartAdvertisingPending &&
      !deviceConnected &&
      millis() - disconnectedAt >=
          ADVERTISING_RESTART_DELAY_MS
  ) {

    restartAdvertisingPending =
        false;

    BLEDevice::startAdvertising();

    Serial.println(
        "[BLE] Advertising restarted"
    );
  }

  char sentence[MAX_BLE_TEXT_PAYLOAD + 1];
  bool hasSentence = false;

  portENTER_CRITICAL(&sentenceMux);
  if (sentenceReady) {
    memcpy(sentence, pendingSentence, sizeof(sentence));
    sentenceReady = false;
    hasSentence = true;
  }
  portEXIT_CRITICAL(&sentenceMux);

  if (hasSentence && sentenceHandler != nullptr) {
    sentenceHandler(String(sentence));
  }
}


// ── Debug logging ─────────────────────────────────────────────────────────────

void transportSendLog(
    const String& msg
) {
  Serial.println(msg);
}
