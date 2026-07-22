# XIAO audio + display pipeline

End-to-end wiring of mic capture -> laptop -> transcription -> OLED display,
over USB serial. See `firmware/oled_mic_streamer/` (Arduino) and `python/`
(laptop side).

## Hardware

- Board: Seeed XIAO ESP32S3
- Displays: 2x 0.42" 72x40 SSD1306 OLEDs, both at I2C 0x3C (mirrored).
  SDA=D4 (GPIO5), SCL=D5 (GPIO6), 3V3.
- Mic 1: INMP441-style I2S mic, I2S port 0. SCK=D0 (GPIO1), WS=D1 (GPIO2),
  SD=D2 (GPIO3), L/R->GND (left channel), 3V3.
- Planned, not yet wired: mic 2 shares I2S0 with L/R->3V3 (right channel);
  mics 3+4 on I2S port 1 (SCK=D3/GPIO4, WS=D8/GPIO7, SD=D9/GPIO8).

## Arduino setup

Board package `XIAO_ESP32S3`, USB CDC On Boot = Enabled. Open
`firmware/oled_mic_streamer/oled_mic_streamer.ino` (Arduino IDE will show
the other files in the folder as tabs) and flash it.

## Running the bridge

Speech-to-text is real Deepgram live transcription (same setup as
`../ble_receiver.py` and the iOS app in `../TranscriberGlasses/`), so you
need a Deepgram API key:

```
cd python
pip install -r requirements.txt
```

Create `python/.env` (already gitignored) with:

```
DEEPGRAM_API_KEY=your_key_here
```

Then:

```
python serial_bridge.py COM5      # replace with the XIAO's port
```

It prints device log lines as `[ESP32] ...`, and each finalized utterance
Deepgram returns is printed as `[STT] -> <sentence>` and sent back to the
device, which draws it on the OLEDs.

> **Note:** `TranscriberGlasses/TranscriberGlasses/ContentView.swift` has a
> Deepgram API key hardcoded and committed to git. Rotate that key on the
> Deepgram dashboard and move it out of source when you get a chance -
> don't reuse it here.

## Wire protocol

Both directions share one framed packet format (`protocol.h` /
`protocol.py`), which is what lets binary audio, text sentences, and debug
logging coexist on a single stream without corrupting each other:

```
[SYNC0=0xAA][SYNC1=0x55][TYPE][LEN_LO][LEN_HI][...LEN payload bytes...][CHECKSUM]
```

`CHECKSUM` is the XOR of `TYPE`, `LEN_LO`, `LEN_HI`, and every payload byte.
A corrupt packet is just dropped; the parser resyncs on the next
`SYNC0`/`SYNC1` pair, so line noise or a mid-packet USB hiccup can't wedge
the link.

| Type | Direction       | Payload                              |
|------|-----------------|---------------------------------------|
| 0x01 AUDIO    | device -> host | raw PCM16 mono samples, little-endian |
| 0x02 SENTENCE | host -> device | one sentence, UTF-8 text              |
| 0x03 LOG      | device -> host | human-readable debug text             |

Because logging is framed too (`transportSendLog`, used everywhere the old
sketch called `Serial.print`/`println`), there's no unframed text on the
wire to be confused with audio - the firmware never writes to `Serial`
outside of `transport_serial.cpp`.

## Swapping in Bluetooth later

`firmware/oled_mic_streamer/transport.h` declares the only four functions
the rest of the firmware uses to move bytes: `transportInit`,
`transportPoll`, `transportSendAudio`, `transportSendLog`. The display code
and `mic.cpp` never touch `Serial` directly. Adding Bluetooth means writing
a `transport_ble.cpp` with the same four functions (still emitting/parsing
the same framed packets from `protocol.h`) and swapping which `.cpp` gets
compiled - no changes needed to the mic or display logic.
