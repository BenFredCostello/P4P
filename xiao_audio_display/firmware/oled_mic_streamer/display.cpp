#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace {

// The 72x40 panel shows this window of the SSD1306's 128x64 RAM.
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int VISIBLE_X = 28;
constexpr int VISIBLE_Y = 24;
constexpr int VISIBLE_WIDTH = 72;
constexpr int VISIBLE_HEIGHT = 40;
constexpr int TEXT_Y_OFFSET = 4;
constexpr int CHAR_WIDTH = 6;
constexpr int CHARS_PER_LINE = 10;
constexpr int MAX_LINES = 4;
constexpr int LINE_HEIGHT = 8;
constexpr uint8_t OLED_ADDRESS = 0x3C;

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledReady = false;

uint8_t displayI2cStatus() {
  Wire.beginTransmission(OLED_ADDRESS);
  return Wire.endTransmission();
}

void flipVisibleImageVertically() {
  for (int y = 0; y < VISIBLE_HEIGHT / 2; y++) {
    int topY = VISIBLE_Y + y;
    int bottomY = VISIBLE_Y + VISIBLE_HEIGHT - 1 - y;
    for (int x = VISIBLE_X; x < VISIBLE_X + VISIBLE_WIDTH; x++) {
      bool top = oled.getPixel(x, topY);
      bool bottom = oled.getPixel(x, bottomY);
      oled.drawPixel(x, topY, bottom ? SSD1306_WHITE : SSD1306_BLACK);
      oled.drawPixel(x, bottomY, top ? SSD1306_WHITE : SSD1306_BLACK);
    }
  }
}

}  // namespace

bool displayInit() {
  if (!Wire.begin(5, 6)) {  // XIAO D4/GPIO5 = SDA, D5/GPIO6 = SCL.
    Serial.println("[OLED] I2C start failed");
    return false;
  }

  uint8_t status = displayI2cStatus();
  if (status != 0) {
    Serial.printf("[OLED] No I2C response at 0x3C (code %u)\n", status);
    return false;
  }

  // Wire is already configured above; do not restart it on default pins.
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false);
  if (!oledReady) {
    return false;
  }

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  displayShowSentence("Waiting for text");
  return true;
}

void displayShowSentence(const String& sentence) {
  if (!oledReady) {
    return;
  }

  uint8_t status = displayI2cStatus();
  if (status != 0) {
    Serial.printf("[OLED] I2C response lost (code %u)\n", status);
    return;
  }

  String lines[MAX_LINES];
  int line = 0;
  unsigned int position = 0;

  while (position < sentence.length() && line < MAX_LINES) {
    while (position < sentence.length() && sentence[position] == ' ') {
      position++;
    }
    if (position >= sentence.length()) {
      break;
    }

    int wordEnd = sentence.indexOf(' ', position);
    if (wordEnd < 0) {
      wordEnd = sentence.length();
    }
    String word = sentence.substring(position, wordEnd);

    bool hasText = !lines[line].isEmpty();
    int separator = hasText ? 1 : 0;
    int freeChars = CHARS_PER_LINE - lines[line].length();

    if ((int)word.length() + separator <= freeChars) {
      if (hasText) {
        lines[line] += ' ';
      }
      lines[line] += word;
      position = wordEnd;
    } else {
      int room = freeChars - separator;
      if (room >= 3) {
        if (hasText) {
          lines[line] += ' ';
        }
        lines[line] += word.substring(0, room - 1) + '-';
        position += room - 1;
      }
      line++;
    }
  }

  oled.clearDisplay();
  for (int i = 0; i < MAX_LINES; i++) {
    int lineWidth = lines[i].length() * CHAR_WIDTH;
    int x = VISIBLE_X + (VISIBLE_WIDTH - lineWidth) / 2;
    oled.setCursor(x, VISIBLE_Y + TEXT_Y_OFFSET + i * LINE_HEIGHT);
    oled.print(lines[i]);
  }
  // The lens flips the image vertically; reverse the visible pixels first.
  flipVisibleImageVertically();
  oled.display();
}
