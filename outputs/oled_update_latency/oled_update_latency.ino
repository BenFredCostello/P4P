/*
  STANDALONE SOUND-TRIGGERED HARDWARE TIMING TEST - XIAO ESP32S3
  Requires Adafruit SSD1306 and Adafruit GFX. No project imports or BLE.
  No oscilloscope, photodiode, or external timing equipment is used.

  MAIN TIMER:
    START: micros() immediately after an audio block crosses the threshold.
    Draw the fixed caption in RAM, then call oled.display().
    STOP: micros() immediately after oled.display() returns.
    Includes caption drawing and ESP32-to-OLED transfer.
    Excludes microphone capture, I2S buffering/read and peak calculation,
    which occur BEFORE the sound-detection timestamp. Also excludes the
    physical OLED response after the transfer and all BLE/cloud processing.
    This is detection-to-transfer-completion, NOT physical sound-to-light.

  A second timer measures oled.display() entry to return: transfer only.
  On the FIRST above-threshold 90-sample block, update the display.
  There is NO silence-confirmation delay. Interrupts stay enabled.

  Serial Monitor 115200 baud:
    c = toggle idle microphone peak readings for threshold calibration
    a = clear OLED and arm one trial; then produce ONE short sound burst
    x = cancel the armed trial
    p = print all stored software timings and statistics
    h = instructions
  Run 100 separate sound bursts/trials. Values remain in RAM until reset.
  No Serial output during an armed trial or during its display update.
  A timeout or capture error cancels the trial without storing a timing.
*/
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

constexpr int SDA_PIN = 5;
constexpr int SCL_PIN = 6;
constexpr uint8_t OLED_ADDRESS = 0x3C;
// Numeric assignments match current mic.cpp, whose pin comments are stale.
constexpr int MIC_SCK_PIN = 1;
constexpr int MIC_WS_PIN = 2;
constexpr int MIC_SD_PIN = 8;       // XIAO D9.
constexpr int MIC_POWER_PIN = 3;    // XIAO D2.
constexpr bool USE_GPIO_MIC_POWER = true;
constexpr i2s_port_t MIC_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t BLOCK_SAMPLES = 90;
constexpr int32_t MIC_GAIN = 16;
// Starting value only. Use 'c', compare quiet/burst peaks, adjust and upload.
constexpr int32_t AUDIO_THRESHOLD = 4000;
constexpr uint32_t ARM_TIMEOUT_MS = 20000;
constexpr uint32_t ARM_SETTLE_MS = 200;

constexpr int BUFFER_WIDTH = 128;
constexpr int BUFFER_HEIGHT = 64;
constexpr int VISIBLE_X = 28;
constexpr int VISIBLE_Y = 24;
constexpr int VISIBLE_WIDTH = 72;
constexpr int TEXT_Y_OFFSET = 4;
constexpr int LINE_HEIGHT = 8;
constexpr int CHAR_WIDTH = 6;
constexpr uint32_t TRANSFER_CLOCK_HZ = 400000;
constexpr uint32_t IDLE_CLOCK_HZ = 100000;
constexpr size_t TRIAL_COUNT = 100;

Adafruit_SSD1306 oled(BUFFER_WIDTH, BUFFER_HEIGHT, &Wire, -1,
                      TRANSFER_CLOCK_HZ, IDLE_CLOCK_HZ);
struct Measurement {
  uint32_t detectionToReturnUs;
  uint32_t transferUs;
};
Measurement measurements[TRIAL_COUNT];
size_t trialCount = 0;
int32_t rawSamples[BLOCK_SAMPLES * 2];
bool ready = false;
bool armed = false;
bool showPeaks = false;
uint32_t armedMs = 0;
uint32_t lastPeakPrintMs = 0;

void prepareCaption() {
  // Deliberately done AFTER sound detection, so drawing is included in
  // the main detection-to-transfer-completion measurement.
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  const char* lines[] = {"weather is", "good", "today"};
  for (int line = 0; line < 3; ++line) {
    const int width = static_cast<int>(strlen(lines[line])) * CHAR_WIDTH;
    oled.setCursor(VISIBLE_X + (VISIBLE_WIDTH - width) / 2,
                   VISIBLE_Y + TEXT_Y_OFFSET + line * LINE_HEIGHT);
    oled.print(lines[line]);
  }
}

bool readPeak(int32_t& peak) {
  size_t bytesRead = 0;
  const esp_err_t result = i2s_read(MIC_PORT, rawSamples, sizeof(rawSamples),
                                  &bytesRead, pdMS_TO_TICKS(20));
  if (result != ESP_OK || bytesRead == 0 || bytesRead % 8 != 0) return false;
  peak = 0;
  for (size_t i = 0; i < bytesRead / 8; ++i) {
    int32_t sample = (rawSamples[i * 2 + 1] >> 16) * MIC_GAIN;
    sample = constrain(sample, -32768, 32767);
    const int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > peak) peak = magnitude;
  }
  return true;
}

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
  if (installed != ESP_OK) return false;
  return i2s_set_pin(MIC_PORT, &pins) == ESP_OK;
}

void printMetric(const char* label, double us) {
  Serial.printf("%s: %.3f us | %.6f ms\n", label, us, us / 1000.0);
}

void printSummary(bool transferOnly) {
  double sum = 0;
  uint32_t minimum = UINT32_MAX;
  uint32_t maximum = 0;
  for (size_t i = 0; i < trialCount; ++i) {
    const uint32_t value = transferOnly ? measurements[i].transferUs
                                       : measurements[i].detectionToReturnUs;
    sum += value;
    if (value < minimum) minimum = value;
    if (value > maximum) maximum = value;
  }
  const double mean = sum / trialCount;
  double squares = 0;
  for (size_t i = 0; i < trialCount; ++i) {
    const double value = transferOnly ? measurements[i].transferUs
                                     : measurements[i].detectionToReturnUs;
    const double difference = value - mean;
    squares += difference * difference;
  }
  Serial.println(transferOnly ? "\nBuffer transfer only:" : "\nDetection to display() return:");
  printMetric("Mean", mean);
  printMetric("Minimum", minimum);
  printMetric("Maximum", maximum);
  if (trialCount > 1) printMetric("Sample standard deviation", sqrt(squares / (trialCount - 1)));
  else Serial.println("Sample standard deviation: requires two trials.");
}

void printResults() {
  if (!trialCount) { Serial.println("No completed trials."); return; }
  Serial.println("trial,detection_to_return_us,detection_to_return_ms,transfer_us,transfer_ms");
  for (size_t i = 0; i < trialCount; ++i) {
    Serial.printf("%u,%lu,%.6f,%lu,%.6f\n", static_cast<unsigned>(i + 1),
                  static_cast<unsigned long>(measurements[i].detectionToReturnUs),
                  measurements[i].detectionToReturnUs / 1000.0,
                  static_cast<unsigned long>(measurements[i].transferUs),
                  measurements[i].transferUs / 1000.0);
  }
  printSummary(false);
  printSummary(true);
  Serial.println("Timers end at transfer completion; physical sound arrival and pixel response are not measured.");
}

void printHelp() {
  Serial.println("\nStandalone sound-triggered OLED test, no BLE or cloud.");
  Serial.println("a=arm ONE burst, c=idle peaks, p=stored timings, x=cancel, h=help");
  Serial.printf("Threshold=%ld; 90 samples at 16 kHz; I2C=400 kHz; capacity=100 trials\n",
                static_cast<long>(AUDIO_THRESHOLD));
  Serial.println("Main timer: detected sound -> draw caption -> OLED transfer returns.");
  Serial.println("Stay quiet until [ARM], then produce one short controlled sound burst.");
}

void handleSerial() {
  while (Serial.available()) {
    const char command = Serial.read();
    if (armed) {
      if (command == 'x') {
        armed = false;
        Serial.println("[CANCELLED] No measurement stored.");
      }
      continue;
    }
    if (command == 'a') {
      if (trialCount >= TRIAL_COUNT) {
        Serial.println("Storage full. Send 'p', then reset to repeat.");
        continue;
      }
      showPeaks = false;
      oled.clearDisplay();
      oled.display(); // Clear previous light BEFORE arming, outside timing.
      // Keep consuming RX data during settling so old sound cannot trigger.
      const uint32_t settleStart = millis();
      int32_t peak = 0;
      bool captureOk = true;
      while (millis() - settleStart < ARM_SETTLE_MS) {
        if (!readPeak(peak)) { captureOk = false; break; }
      }
      if (!captureOk || peak >= AUDIO_THRESHOLD) {
        Serial.println("[NOT ARMED] Check microphone and threshold; stay quiet.");
        continue;
      }
      Serial.println("[ARM] Produce one short sound burst.");
      Serial.flush(); // Complete printing BEFORE the trial becomes active.
      armedMs = millis();
      armed = true;
    } else if (command == 'c') {
      showPeaks = !showPeaks;
      Serial.printf("[CALIBRATION] Peak output %s\n", showPeaks ? "ON" : "OFF");
    } else if (command == 'p') printResults();
    else if (command == 'h') printHelp();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  if (!Wire.begin(SDA_PIN, SCL_PIN)) {
    Serial.println("ERROR: I2C setup failed."); return;
  }
  Wire.setClock(IDLE_CLOCK_HZ);
  Wire.beginTransmission(OLED_ADDRESS);
  if (Wire.endTransmission() != 0 ||
      !oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false)) {
    Serial.println("ERROR: OLED not ready at 0x3C. Check wiring."); return;
  }
  oled.clearDisplay();
  oled.display();
  if (!initMicrophone()) {
    Serial.println("ERROR: I2S microphone setup failed."); return;
  }
  ready = true;
  printHelp();
}

void loop() {
  if (!ready) { delay(20); return; }
  handleSerial();
  int32_t peak = 0;
  if (!readPeak(peak)) {
    if (armed) {
      armed = false;
      Serial.println("[CAPTURE ERROR] No measurement stored.");
    }
    return;
  }
  if (armed && peak >= AUDIO_THRESHOLD) {
    // ---------- LOCAL SOFTWARE TIMING ----------
    // Detection timestamp is AFTER I2S read and peak calculation. It does
    // NOT timestamp physical sound arrival or microphone acquisition.
    const uint32_t detectedUs = micros();
    prepareCaption();
    const uint32_t transferStartUs = micros();
    oled.display();
    const uint32_t transferEndUs = micros();
    // -------- END LOCAL SOFTWARE TIMING --------
    measurements[trialCount++] = {transferEndUs - detectedUs,
                                  transferEndUs - transferStartUs};
    armed = false;
    // All printing is AFTER the trial and display transfer have finished.
    const Measurement& result = measurements[trialCount - 1];
    Serial.printf("[TRIAL %u] detection-to-return=%lu us (%.3f ms); transfer=%lu us (%.3f ms)\n",
                  static_cast<unsigned>(trialCount),
                  static_cast<unsigned long>(result.detectionToReturnUs),
                  result.detectionToReturnUs / 1000.0,
                  static_cast<unsigned long>(result.transferUs),
                  result.transferUs / 1000.0);
    if (trialCount == TRIAL_COUNT) printResults();
    else Serial.println("Send 'a' to clear and arm the next trial.");
  } else if (armed && millis() - armedMs >= ARM_TIMEOUT_MS) {
    armed = false;
    Serial.println("[TIMEOUT] No sound detected; no measurement stored.");
  } else if (!armed && showPeaks && millis() - lastPeakPrintMs >= 500) {
    lastPeakPrintMs = millis();
    Serial.printf("[PEAK] %ld | threshold=%ld\n", static_cast<long>(peak),
                  static_cast<long>(AUDIO_THRESHOLD));
  }
}
