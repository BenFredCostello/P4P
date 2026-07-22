// USB-serial implementation of transport.h. See that file for the
// interface contract; see protocol.h for the framing this parses/emits.

#include "transport.h"
#include "protocol.h"

static SentenceHandler sentenceHandler = nullptr;

// ---- outgoing -------------------------------------------------------------

static void sendPacket(uint8_t type, const uint8_t *payload, size_t len) {
  if (len > PKT_MAX_PAYLOAD) len = PKT_MAX_PAYLOAD;

  uint8_t checksum = type ^ (uint8_t)(len & 0xFF) ^ (uint8_t)((len >> 8) & 0xFF);
  for (size_t i = 0; i < len; i++) checksum ^= payload[i];

  Serial.write(PKT_SYNC0);
  Serial.write(PKT_SYNC1);
  Serial.write(type);
  Serial.write((uint8_t)(len & 0xFF));
  Serial.write((uint8_t)((len >> 8) & 0xFF));
  if (len > 0) Serial.write(payload, len);
  Serial.write(checksum);
}

void transportSendAudio(const uint8_t *data, size_t len) {
  sendPacket(PKT_TYPE_AUDIO, data, len);
}

void transportSendLog(const String &msg) {
  sendPacket(PKT_TYPE_LOG, (const uint8_t *)msg.c_str(), msg.length());
}

// ---- incoming ---------------------------------------------------------
// Small non-blocking state machine so transportPoll() never stalls loop().

enum ParseState {
  WAIT_SYNC0,
  WAIT_SYNC1,
  READ_TYPE,
  READ_LEN_LO,
  READ_LEN_HI,
  READ_PAYLOAD,
  READ_CHECKSUM
};

static ParseState state = WAIT_SYNC0;
static uint8_t pktType;
static uint16_t pktLen;
static uint16_t pktIndex;
static uint8_t pktBuf[PKT_MAX_PAYLOAD];
static uint8_t runningChecksum;

static void handlePacket(uint8_t type, const uint8_t *payload, uint16_t len) {
  if (type == PKT_TYPE_SENTENCE && sentenceHandler != nullptr) {
    String s;
    s.reserve(len);
    for (uint16_t i = 0; i < len; i++) s += (char)payload[i];
    sentenceHandler(s);
  }
  // AUDIO/LOG are device->host only; silently ignore if ever seen incoming.
}

void transportInit(unsigned long baud, SentenceHandler onSentence) {
  sentenceHandler = onSentence;
  Serial.begin(baud);
  state = WAIT_SYNC0;
}

void transportPoll() {
  while (Serial.available() > 0) {
    uint8_t b = (uint8_t)Serial.read();

    switch (state) {
      case WAIT_SYNC0:
        if (b == PKT_SYNC0) state = WAIT_SYNC1;
        break;

      case WAIT_SYNC1:
        state = (b == PKT_SYNC1) ? READ_TYPE : WAIT_SYNC0;
        break;

      case READ_TYPE:
        pktType = b;
        runningChecksum = b;
        state = READ_LEN_LO;
        break;

      case READ_LEN_LO:
        pktLen = b;
        runningChecksum ^= b;
        state = READ_LEN_HI;
        break;

      case READ_LEN_HI:
        pktLen |= ((uint16_t)b << 8);
        runningChecksum ^= b;
        if (pktLen > PKT_MAX_PAYLOAD) {
          state = WAIT_SYNC0;  // bogus length, resync
        } else {
          pktIndex = 0;
          state = (pktLen == 0) ? READ_CHECKSUM : READ_PAYLOAD;
        }
        break;

      case READ_PAYLOAD:
        pktBuf[pktIndex++] = b;
        runningChecksum ^= b;
        if (pktIndex >= pktLen) state = READ_CHECKSUM;
        break;

      case READ_CHECKSUM:
        if (b == runningChecksum) handlePacket(pktType, pktBuf, pktLen);
        state = WAIT_SYNC0;
        break;
    }
  }
}
