"""Wire protocol shared with firmware/oled_mic_streamer/protocol.h.

Every message on the link (either direction) is a framed packet, so binary
audio, text sentences, and debug logging can share one stream without
colliding:

    [SYNC0][SYNC1][TYPE][LEN_LO][LEN_HI][...LEN bytes of payload...][CHECKSUM]

CHECKSUM = TYPE ^ LEN_LO ^ LEN_HI ^ every payload byte, XORed together.
A bad checksum just drops that one packet; the parser resyncs on the next
SYNC0/SYNC1 pair.
"""

SYNC0 = 0xAA
SYNC1 = 0x55
MAX_PAYLOAD = 2048

TYPE_AUDIO = 0x01     # device -> host: raw PCM16 mono samples, little-endian
TYPE_SENTENCE = 0x02  # host -> device: one transcribed sentence, UTF-8 text
TYPE_LOG = 0x03       # device -> host: human-readable debug text


def build_packet(msg_type: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large ({len(payload)} > {MAX_PAYLOAD})")
    length = len(payload)
    checksum = msg_type ^ (length & 0xFF) ^ ((length >> 8) & 0xFF)
    for b in payload:
        checksum ^= b
    header = bytes([SYNC0, SYNC1, msg_type, length & 0xFF, (length >> 8) & 0xFF])
    return header + payload + bytes([checksum])


class PacketParser:
    """Incremental parser: feed() raw bytes as they arrive, get back a list
    of (msg_type, payload) tuples for every complete, checksum-valid packet.
    Resyncs automatically on garbage or a bad checksum."""

    (_WAIT_SYNC0, _WAIT_SYNC1, _READ_TYPE, _READ_LEN_LO,
     _READ_LEN_HI, _READ_PAYLOAD, _READ_CHECKSUM) = range(7)

    def __init__(self):
        self._state = self._WAIT_SYNC0
        self._type = 0
        self._len = 0
        self._payload = bytearray()
        self._checksum = 0

    def feed(self, data: bytes):
        packets = []
        for b in data:
            if self._state == self._WAIT_SYNC0:
                if b == SYNC0:
                    self._state = self._WAIT_SYNC1

            elif self._state == self._WAIT_SYNC1:
                self._state = self._READ_TYPE if b == SYNC1 else self._WAIT_SYNC0

            elif self._state == self._READ_TYPE:
                self._type = b
                self._checksum = b
                self._state = self._READ_LEN_LO

            elif self._state == self._READ_LEN_LO:
                self._len = b
                self._checksum ^= b
                self._state = self._READ_LEN_HI

            elif self._state == self._READ_LEN_HI:
                self._len |= (b << 8)
                self._checksum ^= b
                self._payload = bytearray()
                if self._len > MAX_PAYLOAD:
                    self._state = self._WAIT_SYNC0  # bogus length, resync
                elif self._len == 0:
                    self._state = self._READ_CHECKSUM
                else:
                    self._state = self._READ_PAYLOAD

            elif self._state == self._READ_PAYLOAD:
                self._payload.append(b)
                self._checksum ^= b
                if len(self._payload) >= self._len:
                    self._state = self._READ_CHECKSUM

            elif self._state == self._READ_CHECKSUM:
                if b == self._checksum:
                    packets.append((self._type, bytes(self._payload)))
                self._state = self._WAIT_SYNC0

        return packets
