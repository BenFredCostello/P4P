"""Bridges the XIAO ESP32S3 firmware over USB serial to Deepgram's live
transcription API, and sends transcribed sentences back to the device to
be drawn on the OLEDs.

Same Deepgram live-transcription setup as ../../ble_receiver.py and
../../TranscriberGlasses/TranscriberGlasses/BluetoothManager.swift, just
fed from framed serial audio packets instead of BLE notifications.

Requires a Deepgram API key. Create a `.env` file next to this script
(already covered by .gitignore) containing:

    DEEPGRAM_API_KEY=your_key_here

Usage:
    python serial_bridge.py COM5
"""

import argparse
import os
import struct
import threading
import time

import serial
from dotenv import load_dotenv
from deepgram import (
    DeepgramClient,
    LiveTranscriptionEvents,
    LiveOptions,
)

from protocol import PacketParser, build_packet, TYPE_AUDIO, TYPE_LOG, TYPE_SENTENCE

SAMPLE_RATE = 16000

load_dotenv()
DEEPGRAM_API_KEY = os.getenv("DEEPGRAM_API_KEY")


def start_deepgram(on_sentence):
    """Opens a Deepgram live-transcription websocket. Calls
    on_sentence(text) once per finalized utterance. Blocks until the
    connection is open (or times out)."""
    if not DEEPGRAM_API_KEY:
        raise RuntimeError(
            "DEEPGRAM_API_KEY not set - create a .env file next to this "
            "script with DEEPGRAM_API_KEY=your_key_here"
        )

    deepgram = DeepgramClient(DEEPGRAM_API_KEY)
    dg_connection = deepgram.listen.websocket.v("1")
    ready = threading.Event()

    def on_transcript(self, result, **kwargs):
        try:
            if not result.is_final:
                return
            transcript = result.channel.alternatives[0].transcript
            if transcript.strip():
                on_sentence(transcript.strip())
        except Exception as e:
            print(f"[Deepgram] transcript handling error: {e}")

    def on_open(self, open, **kwargs):
        print("[Deepgram] connection open, streaming...")
        ready.set()

    def on_error(self, error, **kwargs):
        print(f"[Deepgram] error: {error}")

    def on_close(self, close, **kwargs):
        print("[Deepgram] connection closed")

    dg_connection.on(LiveTranscriptionEvents.Open, on_open)
    dg_connection.on(LiveTranscriptionEvents.Transcript, on_transcript)
    dg_connection.on(LiveTranscriptionEvents.Error, on_error)
    dg_connection.on(LiveTranscriptionEvents.Close, on_close)

    options = LiveOptions(
        model="nova-3",
        language="en",
        encoding="linear16",
        sample_rate=SAMPLE_RATE,
        channels=1,
        interim_results=True,
        utterance_end_ms=1000,
    )
    dg_connection.start(options)

    if not ready.wait(timeout=10):
        print("[Deepgram] warning: didn't confirm open within 10s, continuing anyway")

    return dg_connection


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", help="Serial port, e.g. COM5")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    parser = PacketParser()

    def on_sentence(text):
        print(f"[STT] -> {text}")
        ser.write(build_packet(TYPE_SENTENCE, text.encode("utf-8")))

    print("Connecting to Deepgram...")
    dg_connection = start_deepgram(on_sentence)

    print(f"Connected to {args.port} @ {args.baud}")

    # TEMP DEBUG: print mic peak amplitude every ~0.5s so we can sanity-check
    # the signal (int16 range is +/-32768) before trusting Deepgram's output.
    last_peak_print = 0.0

    try:
        while True:
            chunk = ser.read(4096)
            if not chunk:
                continue

            for msg_type, payload in parser.feed(chunk):
                if msg_type == TYPE_AUDIO:
                    dg_connection.send(payload)

                    samples = struct.unpack(f"{len(payload) // 2}h", payload)
                    peak = max(abs(s) for s in samples) if samples else 0
                    now = time.time()
                    if now - last_peak_print > 0.5:
                        print(f"[DEBUG] audio peak: {peak} / 32768")
                        last_peak_print = now

                elif msg_type == TYPE_LOG:
                    print(f"[ESP32] {payload.decode('utf-8', errors='replace')}")
                # TYPE_SENTENCE arriving from the device would be unexpected; ignore.

    except KeyboardInterrupt:
        print("\nExiting.")
    finally:
        dg_connection.finish()
        ser.close()


if __name__ == "__main__":
    main()
