#pragma once
#include <stdint.h>

// Wire protocol shared by the firmware and python/protocol.py.
//
// Every message on the link (either direction) is a framed packet, so
// binary audio, text sentences, and debug logging can share one stream
// without colliding or needing separate raw-text conventions:
//
//   [SYNC0][SYNC1][TYPE][LEN_LO][LEN_HI][...LEN bytes of payload...][CHECKSUM]
//
// CHECKSUM = TYPE ^ LEN_LO ^ LEN_HI ^ every payload byte, XORed together.
// A bad checksum just drops that one packet and resyncs on the next
// SYNC0/SYNC1 pair - no protocol-level recovery needed.
//
// This same framing works unchanged over a future BLE transport (it's just
// bytes in/out), which is why it lives here rather than inside transport.h.

#define PKT_SYNC0 0xAA
#define PKT_SYNC1 0x55
#define PKT_MAX_PAYLOAD 2048

#define PKT_TYPE_AUDIO    0x01  // device -> host: raw PCM16 mono samples, little-endian
#define PKT_TYPE_SENTENCE 0x02  // host -> device: one transcribed sentence, UTF-8 text
#define PKT_TYPE_LOG      0x03  // device -> host: human-readable debug text
