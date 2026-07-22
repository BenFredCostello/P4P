#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "protocol.h"
#include "transport.h"
#include "mic.h"

// Panel is 72x40 but the SSD1306 controller has 128x64 RAM.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

#define X_OFF 28
#define Y_OFF 24
#define VIS_W 72
#define VIS_H 40

#define CHARS_PER_LINE 12
#define MAX_LINES 4
#define LINE_HEIGHT 8
#define MIN_SPLIT 2

#define CHECK_INTERVAL_MS 500  // how often we probe the display connection

#define SERIAL_BAUD 921600     // USB CDC ignores the literal rate, but pyserial wants a value
#define AUDIO_CHUNK_SAMPLES 256

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

bool displayReady = false;
String currentSentence = "";  // last sentence requested, redrawn on reconnect
unsigned long lastCheck = 0;

// Quick I2C probe: is anything ACKing at the display address right now?
bool displayConnected() {
  Wire.beginTransmission(OLED_ADDR);
  return (Wire.endTransmission() == 0);
}

// Word-wraps one sentence across up to MAX_LINES lines, hyphenating
// words that overhang a line. Extra text past line 4 is dropped.
void renderSentence(const String &text) {
  String lines[MAX_LINES];
  int line = 0;
  unsigned int pos = 0;

  while (pos < text.length() && line < MAX_LINES) {
    while (pos < text.length() && text[pos] == ' ') pos++;
    if (pos >= text.length()) break;

    int wordEnd = text.indexOf(' ', pos);
    if (wordEnd == -1) wordEnd = text.length();
    String word = text.substring(pos, wordEnd);

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

// Public entry point: call this with each new sentence from the mic reader.
// Always remembers the sentence; only draws if the display is alive.
void drawSentence(const String &text) {
  currentSentence = text;
  if (displayReady) {
    renderSentence(text);
  } else {
    transportSendLog("(display offline - sentence stored, will draw on reconnect)");
  }
}

// Try to bring the display up. Returns true on success.
bool initDisplay() {
  if (!displayConnected()) return false;

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) return false;

  // startup flash so you can see it came alive
  display.clearDisplay();
  display.fillRect(X_OFF, Y_OFF, VIS_W, VIS_H, SSD1306_WHITE);
  display.display();
  delay(300);

  displayReady = true;
  transportSendLog("Display connected!");

  // redraw whatever the latest sentence is
  if (currentSentence.length() > 0) renderSentence(currentSentence);
  else { display.clearDisplay(); display.display(); }

  return true;
}

// Called by the transport layer whenever a full PKT_TYPE_SENTENCE packet
// arrives from the host (Python side).
void onSentenceReceived(const String &sentence) {
  drawSentence(sentence);
}

void setup() {
  transportInit(SERIAL_BAUD, onSentenceReceived);
  delay(1500);  // let USB CDC enumerate before we start chattering
  transportSendLog("=== OLED + MIC STREAMER RUNNING ===");

  Wire.begin();  // XIAO defaults: D4=SDA (GPIO5), D5=SCL (GPIO6)

  // test input - later this comes from actual transcriptions
  currentSentence = "Testing the microphone sentence display";

  // NOTE: no while(true) death loop. If the display isn't there yet, loop()
  // keeps trying and complaining.
  initDisplay();

  if (!micInit()) {
    transportSendLog("Mic init FAILED");
  } else {
    transportSendLog("Mic ready (I2S0, 16kHz mono)");
  }
}

void loop() {
  unsigned long now = millis();
  if (now - lastCheck >= CHECK_INTERVAL_MS) {
    lastCheck = now;

    bool connected = displayConnected();

    if (!displayReady && connected) {
      // it just appeared (or wiring got wiggled into place) - bring it up
      initDisplay();
    } else if (!displayReady && !connected) {
      // the constant nag you asked for - fires every half second
      transportSendLog("Display not found!");
    } else if (displayReady && !connected) {
      // was working, connection dropped (loose wire etc.)
      transportSendLog("Display connection LOST - waiting for it to come back...");
      displayReady = false;
    }
    // displayReady && connected: all good, do nothing
  }

  // service incoming sentence packets (host -> device)
  transportPoll();

  // pull one chunk of mic audio and ship it out (device -> host)
  static int16_t audioBuf[AUDIO_CHUNK_SAMPLES];
  size_t n = micReadChunk(audioBuf, AUDIO_CHUNK_SAMPLES);
  if (n > 0) {
    transportSendAudio((const uint8_t *)audioBuf, n * sizeof(int16_t));
  }
}
