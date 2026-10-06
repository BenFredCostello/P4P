/*
  STANDALONE BLE/CLOUD RETURN LATENCY TEST - XIAO ESP32S3

  No local project files are required or modified. No OLED is used.
  Use the existing iPhone app (or laptop ble_testing.py) as the BLE/cloud
  bridge. This sketch cannot perform cloud recognition without a bridge.

  Serial Monitor: 115200 baud. Commands:
    a : arm ONE trial, then say "weather is good today" and remain quiet
    c : toggle idle microphone peak readings to choose a threshold
    p : print all stored attempts and statistics for valid trials
    x : cancel the current trial
    h : print instructions

  Up to 100 attempts are stored in RAM. Reset clears them.
  Wait for the phone to finish setup and any old captions before arming.
  Do not speak another sentence until the trial finishes.

  START: micros() timestamp at the end of the FIRST below-threshold
         audio block after detected speech. Confirm only after continuous
         quiet for SILENCE_CONFIRM_MS. If speech resumes, reset candidate.
  STOP:  micros() at entry to the BLE text-write callback containing the
         whole word "today". Matching is case-insensitive.
  RESULT: signed stop-start, using the ORIGINAL silence-start timestamp.
          No silence-confirmation delay is added or subtracted manually.

  Includes the remaining BLE/phone/cloud/recognition/finalisation/return
  delay after the detected speech end. Excludes display processing and
  OLED response. Speech-end detection is an estimate from block peaks,
  not an exact acoustic word boundary. I2S/DMA buffering also affects
  the relationship between this timestamp and the physical sound.
  The callback entry time is after the BLE stack has dispatched the write.

  No Serial printing occurs during an active trial or in BLE callbacks.
  Audio capture/streaming continues between trials and while waiting for
  the caption, so the cloud service receives the intervening silence.
  Interrupts remain enabled. No timestamps on the phone are needed.
*/

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <driver/i2s.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// Numeric pin assignments match the latest modular mic.cpp.
// GPIO8 is XIAO D9; GPIO3 is D2. Earlier comments/README differ.
constexpr int MIC_SCK_PIN = 1;
constexpr int MIC_WS_PIN = 2;
constexpr int MIC_SD_PIN = 8;
constexpr int MIC_POWER_PIN = 3;
constexpr bool USE_GPIO_MIC_POWER = true;

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t BLOCK_SAMPLES = 90;  // 180 PCM16 bytes, 5.625 ms of audio.
constexpr int32_t MIC_GAIN = 16;
constexpr size_t MAX_AUDIO_BYTES = 180;
constexpr size_t MAX_TEXT_BYTES = 180;
constexpr size_t MAX_ATTEMPTS = 100;
constexpr i2s_port_t MIC_PORT = I2S_NUM_0;

// Threshold applies to the PEAK magnitude of the gain-adjusted PCM16 block.
// 800 is a starting value, not a calibrated setting. Use 'c' to inspect
// quiet/speech levels, then set a threshold between those levels.
constexpr int32_t AUDIO_THRESHOLD = 1500;
constexpr uint32_t SPEECH_CONFIRM_MS = 30;
constexpr uint32_t SILENCE_CONFIRM_MS = 300;
constexpr uint32_t TRIAL_TIMEOUT_MS = 20000;

constexpr char DEVICE_NAME[] = "ESP32-Audio";
constexpr char SERVICE_UUID[] = "12345678-1234-1234-1234-123456789abc";
constexpr char AUDIO_UUID[] = "12345678-1234-1234-1234-123456789abc";
constexpr char TEXT_UUID[] = "87654321-4321-4321-4321-cba987654321";

enum class Outcome { VALID, TIMEOUT, DISCONNECTED, CANCELLED, IO_ERROR };

// BEGIN PURE MEASUREMENT LOGIC
struct Trial {
  bool active = false;
  bool onsetCandidate = false;
  bool speechStarted = false;
  bool silenceCandidate = false;
  bool silenceConfirmed = false;
  bool targetReceived = false;
  uint32_t armedUs = 0;
  uint32_t onsetUs = 0;
  uint32_t silenceStartUs = 0;
  uint32_t silenceConfirmedUs = 0;
  uint32_t targetReceivedUs = 0;
  uint32_t notifyErrors = 0;
  uint32_t captureErrors = 0;
  char returnedText[MAX_TEXT_BYTES + 1] = {};
};

bool containsToday(const char* text) {
  // Match whole alphabetic tokens, not substrings such as "todayish".
  while (*text) {
    while (*text && !isalpha(static_cast<unsigned char>(*text))) ++text;
    const char* start = text;
    while (*text && isalpha(static_cast<unsigned char>(*text))) ++text;
    if (text - start == 5 &&
        tolower(static_cast<unsigned char>(start[0])) == 's' &&
        tolower(static_cast<unsigned char>(start[1])) == 't' &&
        tolower(static_cast<unsigned char>(start[2])) == 'o' &&
        tolower(static_cast<unsigned char>(start[3])) == 'r' &&
        tolower(static_cast<unsigned char>(start[4])) == 'e') return true;
  }
  return false;
}

void processBlock(Trial& t, int32_t peak, uint32_t blockReadEndUs) {
  if (!t.active) return;

  if (peak >= AUDIO_THRESHOLD) {
    if (!t.speechStarted) {
      if (!t.onsetCandidate) {
        t.onsetCandidate = true;
        t.onsetUs = blockReadEndUs;
      }
      if (static_cast<uint32_t>(blockReadEndUs - t.onsetUs) >=
          SPEECH_CONFIRM_MS * 1000U) t.speechStarted = true;
    }
    // Any above-threshold block cancels a tentative/confirmed pause.
    t.silenceCandidate = false;
    t.silenceConfirmed = false;
    return;
  }

  if (!t.speechStarted) {
    t.onsetCandidate = false;
    return;  // Initial silence must not start the measurement.
  }

  if (!t.silenceCandidate) {
    t.silenceCandidate = true;
    t.silenceStartUs = blockReadEndUs;
  }
  if (!t.silenceConfirmed &&
      static_cast<uint32_t>(blockReadEndUs - t.silenceStartUs) >=
          SILENCE_CONFIRM_MS * 1000U) {
    t.silenceConfirmed = true;
    t.silenceConfirmedUs = blockReadEndUs;
  }
}

void acceptText(Trial& t, const char* text, uint32_t receivedUs) {
  // Accept a target even before silence has been confirmed. Ignore writes
  // before speech onset, outside the trial, and repeat matches.
  if (!t.active || !t.speechStarted || t.targetReceived ||
      !containsToday(text)) return;
  t.targetReceived = true;
  t.targetReceivedUs = receivedUs;
  strncpy(t.returnedText, text, MAX_TEXT_BYTES);
  t.returnedText[MAX_TEXT_BYTES] = '\0';
}

int32_t measuredLatencyUs(const Trial& t) {
  // Signed result permits a caption that arrives before detected silence.
  // Wrap-safe for these trials, which are far shorter than 2^31 us.
  return static_cast<int32_t>(t.targetReceivedUs - t.silenceStartUs);
}
// END PURE MEASUREMENT LOGIC


struct Attempt {
  Trial trial;
  Outcome outcome;
};

Trial currentTrial;
Attempt attempts[MAX_ATTEMPTS];
size_t attemptCount = 0;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
bool connected = false;
bool linkLostDuringTrial = false;
bool connectionEvent = false;
bool flushCapture = false;
bool micReady = false;
bool showPeaks = false;
int32_t lastPeak = 0;
uint32_t lastPeakPrintMs = 0;
BLEServer* bleServer = nullptr;
BLECharacteristic* audioCharacteristic = nullptr;
int32_t rawSamples[BLOCK_SAMPLES * 2];
int16_t pcmSamples[BLOCK_SAMPLES];

class LinkCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    portENTER_CRITICAL(&stateMux);
    connected = true;
    connectionEvent = true;
    flushCapture = true;
    portEXIT_CRITICAL(&stateMux);
  }
  void onDisconnect(BLEServer*) override {
    portENTER_CRITICAL(&stateMux);
    connected = false;
    connectionEvent = true;
    if (currentTrial.active) linkLostDuringTrial = true;
    portEXIT_CRITICAL(&stateMux);
  }
};

class AudioCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic*, Status status, uint32_t) override {
    if (status == SUCCESS_NOTIFY || status == SUCCESS_INDICATE) return;
    portENTER_CRITICAL(&stateMux);
    if (currentTrial.active) ++currentTrial.notifyErrors;
    portEXIT_CRITICAL(&stateMux);
  }
};

class TextCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    // Capture STOP immediately on callback entry, before text extraction.
    const uint32_t receivedUs = micros();
    const String text = characteristic->getValue();
    portENTER_CRITICAL(&stateMux);
    acceptText(currentTrial, text.c_str(), receivedUs);
    portEXIT_CRITICAL(&stateMux);
    // No display calls or Serial output here.
  }
};

bool initMicrophone() {
  if (USE_GPIO_MIC_POWER) {
    pinMode(MIC_POWER_PIN, OUTPUT);
    digitalWrite(MIC_POWER_PIN, HIGH);
    delay(10);
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
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

  const esp_err_t installed = i2s_driver_install(MIC_PORT, &config, 0, nullptr);
  if (installed != ESP_OK) {
    Serial.printf("[ERROR] I2S installation: %d\n", installed);
    return false;
  }
  const esp_err_t assigned = i2s_set_pin(MIC_PORT, &pins);
  if (assigned != ESP_OK) {
    Serial.printf("[ERROR] I2S pin setup: %d\n", assigned);
    return false;
  }
  i2s_zero_dma_buffer(MIC_PORT);
  return true;
}

void initBle() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new LinkCallbacks());
  bleServer->advertiseOnDisconnect(true);
  BLEService* service = bleServer->createService(SERVICE_UUID);
  audioCharacteristic = service->createCharacteristic(
      AUDIO_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  audioCharacteristic->setCallbacks(new AudioCallbacks());
  // NimBLE supplies the notification descriptor automatically.
#if defined(CONFIG_BLUEDROID_ENABLED)
  audioCharacteristic->addDescriptor(new BLE2902());
#endif
  BLECharacteristic* text = service->createCharacteristic(
      TEXT_UUID, BLECharacteristic::PROPERTY_WRITE |
                     BLECharacteristic::PROPERTY_WRITE_NR);
  text->setCallbacks(new TextCallbacks());
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
}

const char* outcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::VALID: return "VALID";
    case Outcome::TIMEOUT: return "TIMEOUT";
    case Outcome::DISCONNECTED: return "DISCONNECTED";
    case Outcome::CANCELLED: return "CANCELLED";
    case Outcome::IO_ERROR: return "IO_ERROR";
  }
  return "UNKNOWN";
}

void printAttempt(size_t index) {
  const Attempt& a = attempts[index];
  const Trial& t = a.trial;
  Serial.printf("[TRIAL %u] %s", static_cast<unsigned>(index + 1),
                outcomeName(a.outcome));
  if (a.outcome == Outcome::VALID) {
    const int32_t latency = measuredLatencyUs(t);
    Serial.printf(" | latency=%ld us (%.3f ms) | confirmation=%.3f ms\n",
                  static_cast<long>(latency), latency / 1000.0,
                  static_cast<uint32_t>(t.silenceConfirmedUs -
                                        t.silenceStartUs) / 1000.0);
    Serial.printf("  Returned text: %s\n", t.returnedText);
    if (static_cast<int32_t>(t.targetReceivedUs - t.silenceConfirmedUs) < 0)
      Serial.println("  Target arrived BEFORE silence confirmation; timestamp retained.");
    if (latency < 0)
      Serial.println("  Caption preceded detected speech end; signed negative value retained.");
  } else {
    Serial.printf(" | speech=%u silence_confirmed=%u target_received=%u"
                  " notify_errors=%lu capture_errors=%lu\n",
                  t.speechStarted, t.silenceConfirmed, t.targetReceived,
                  static_cast<unsigned long>(t.notifyErrors),
                  static_cast<unsigned long>(t.captureErrors));
  }
}

void finishAttempt(Outcome outcome) {
  portENTER_CRITICAL(&stateMux);
  Trial completed = currentTrial;
  currentTrial.active = false;
  linkLostDuringTrial = false;
  portEXIT_CRITICAL(&stateMux);
  // Printing occurs only after measurement has ended or been invalidated.
  if (attemptCount < MAX_ATTEMPTS) {
    attempts[attemptCount] = {completed, outcome};
    printAttempt(attemptCount++);
  }
  Serial.println("Wait for old captions to finish. Send 'a' for the next trial.");
}

void printStoredResults() {
  Serial.println("attempt,status,latency_us,latency_ms");
  size_t validCount = 0;
  double sum = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  for (size_t i = 0; i < attemptCount; ++i) {
    const Attempt& a = attempts[i];
    Serial.printf("%u,%s,", static_cast<unsigned>(i + 1), outcomeName(a.outcome));
    if (a.outcome != Outcome::VALID) {
      Serial.println(",");
      continue;
    }
    const double value = measuredLatencyUs(a.trial);
    Serial.printf("%.0f,%.3f\n", value, value / 1000.0);
    if (validCount == 0 || value < minimum) minimum = value;
    if (validCount == 0 || value > maximum) maximum = value;
    sum += value;
    ++validCount;
  }
  if (validCount == 0) {
    Serial.println("No valid trials stored.");
    return;
  }
  const double mean = sum / validCount;
  double squares = 0.0;
  for (size_t i = 0; i < attemptCount; ++i) {
    if (attempts[i].outcome != Outcome::VALID) continue;
    const double difference = measuredLatencyUs(attempts[i].trial) - mean;
    squares += difference * difference;
  }
  Serial.printf("Valid trials: %u\n", static_cast<unsigned>(validCount));
  Serial.printf("Mean: %.3f us | %.3f ms\n", mean, mean / 1000.0);
  Serial.printf("Minimum: %.0f us | %.3f ms\n", minimum, minimum / 1000.0);
  Serial.printf("Maximum: %.0f us | %.3f ms\n", maximum, maximum / 1000.0);
  if (validCount > 1) {
    const double sd = sqrt(squares / (validCount - 1));
    Serial.printf("Sample standard deviation: %.3f us | %.3f ms\n", sd, sd / 1000.0);
  } else {
    Serial.println("Sample standard deviation: requires at least two valid trials.");
  }
}

void printHelp() {
  Serial.println("\nStandalone BLE/cloud latency test; no OLED output.");
  Serial.println("Connect the phone app, wait for audio/transcription setup, then stay quiet.");
  Serial.println("a=arm trial, c=toggle idle peaks, p=stored results, x=cancel, h=help");
  Serial.println("After ARM, say: weather is good today. Then remain quiet.");
  Serial.printf("Threshold=%ld PCM peak, speech confirmation=%lu ms, silence confirmation=%lu ms\n",
                static_cast<long>(AUDIO_THRESHOLD),
                static_cast<unsigned long>(SPEECH_CONFIRM_MS),
                static_cast<unsigned long>(SILENCE_CONFIRM_MS));
  Serial.printf("Mic pins: SCK=%d WS=%d SD=%d GPIO-power=%d enabled=%u\n",
                MIC_SCK_PIN, MIC_WS_PIN, MIC_SD_PIN, MIC_POWER_PIN, USE_GPIO_MIC_POWER);
}

void handleSerial() {
  while (Serial.available()) {
    const char command = tolower(static_cast<unsigned char>(Serial.read()));
    portENTER_CRITICAL(&stateMux);
    const bool active = currentTrial.active;
    const bool linkUp = connected;
    portEXIT_CRITICAL(&stateMux);
    if (active) {
      if (command == 'x') finishAttempt(Outcome::CANCELLED);
      continue;  // No commands may print while a trial is active.
    }
    if (command == 'a') {
      if (!linkUp || !micReady) {
        Serial.println("[NOT ARMED] Connect the phone and check the microphone first.");
        continue;
      }
      if (attemptCount >= MAX_ATTEMPTS) {
        Serial.println("[NOT ARMED] Storage full. Send 'p', then reset for a new session.");
        continue;
      }
      if (lastPeak >= AUDIO_THRESHOLD) {
        Serial.println("[NOT ARMED] Stay quiet, or adjust the microphone threshold.");
        continue;
      }
      showPeaks = false;
      Serial.println("[ARM] Say: weather is good today. Then remain quiet.");
      Serial.flush();  // Finish prior Serial output BEFORE activating the trial.
      portENTER_CRITICAL(&stateMux);
      currentTrial = Trial{};
      currentTrial.armedUs = micros();
      currentTrial.active = true;
      linkLostDuringTrial = false;
      portEXIT_CRITICAL(&stateMux);
    } else if (command == 'c') {
      showPeaks = !showPeaks;
      Serial.printf("[CALIBRATION] Idle peak output %s.\n", showPeaks ? "ON" : "OFF");
    } else if (command == 'p') {
      printStoredResults();
    } else if (command == 'h') {
      printHelp();
    }
  }
}

void streamAudio(const int16_t* samples, size_t sampleCount) {
  const uint16_t mtu = bleServer->getPeerMTU(bleServer->getConnId());
  size_t payload = mtu > 3 ? mtu - 3 : 20;
  if (payload > MAX_AUDIO_BYTES) payload = MAX_AUDIO_BYTES;
  payload &= ~static_cast<size_t>(1);
  if (payload < 2) return;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(samples);
  size_t remaining = sampleCount * sizeof(int16_t);
  while (remaining) {
    portENTER_CRITICAL(&stateMux);
    const bool linkUp = connected;
    portEXIT_CRITICAL(&stateMux);
    if (!linkUp) return;
    const size_t count = remaining < payload ? remaining : payload;
    audioCharacteristic->setValue(bytes, count);
    audioCharacteristic->notify();
    bytes += count;
    remaining -= count;
    // No additional pacing delay: I2S acquisition already paces the stream.
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  micReady = initMicrophone();
  if (!micReady) return;
  initBle();
  printHelp();
  Serial.println("[BLE] Advertising as ESP32-Audio.");
}

void loop() {
  handleSerial();
  if (!micReady) { delay(20); return; }

  portENTER_CRITICAL(&stateMux);
  const bool linkUp = connected;
  const bool changed = connectionEvent;
  connectionEvent = false;
  const bool lost = linkLostDuringTrial;
  const bool flush = flushCapture;
  flushCapture = false;
  const bool active = currentTrial.active;
  portEXIT_CRITICAL(&stateMux);
  if (lost && active) finishAttempt(Outcome::DISCONNECTED);
  if (changed && !active) Serial.printf("[BLE] %s\n", linkUp ? "Connected" : "Disconnected");
  if (!linkUp) { delay(10); return; }
  if (flush) i2s_zero_dma_buffer(MIC_PORT);

  size_t bytesRead = 0;
  const esp_err_t error = i2s_read(MIC_PORT, rawSamples, sizeof(rawSamples),
                                  &bytesRead, pdMS_TO_TICKS(20));
  // This is the software-observed block time, NOT an acoustic timestamp.
  const uint32_t blockReadEndUs = micros();
  const size_t frames = bytesRead / (2 * sizeof(int32_t));
  if (error != ESP_OK || frames == 0 || bytesRead % 8 != 0) {
    portENTER_CRITICAL(&stateMux);
    if (currentTrial.active) ++currentTrial.captureErrors;
    portEXIT_CRITICAL(&stateMux);
  } else {
    int32_t peak = 0;
    for (size_t i = 0; i < frames; ++i) {
      int32_t sample = (rawSamples[i * 2 + 1] >> 16) * MIC_GAIN;
      sample = constrain(sample, -32768, 32767);
      pcmSamples[i] = static_cast<int16_t>(sample);
      const int32_t magnitude = sample < 0 ? -sample : sample;
      if (magnitude > peak) peak = magnitude;
    }
    lastPeak = peak;
    portENTER_CRITICAL(&stateMux);
    processBlock(currentTrial, peak, blockReadEndUs);
    portEXIT_CRITICAL(&stateMux);
    streamAudio(pcmSamples, frames);
  }

  portENTER_CRITICAL(&stateMux);
  const Trial snapshot = currentTrial;
  const bool linkFailed = linkLostDuringTrial;
  portEXIT_CRITICAL(&stateMux);
  if (snapshot.active) {
    if (linkFailed) finishAttempt(Outcome::DISCONNECTED);
    else if (snapshot.captureErrors) finishAttempt(Outcome::IO_ERROR);
    else if (snapshot.silenceConfirmed && snapshot.targetReceived) finishAttempt(Outcome::VALID);
    else if (static_cast<uint32_t>(micros() - snapshot.armedUs) >=
             TRIAL_TIMEOUT_MS * 1000U) finishAttempt(Outcome::TIMEOUT);
  } else if (showPeaks && millis() - lastPeakPrintMs >= 500) {
    lastPeakPrintMs = millis();
    Serial.printf("[PEAK] %ld | threshold=%ld\n", static_cast<long>(lastPeak),
                  static_cast<long>(AUDIO_THRESHOLD));
  }
}
