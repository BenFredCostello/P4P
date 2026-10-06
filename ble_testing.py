"""BLE microphone -> Deepgram -> XIAO OLED bridge.

Run from the repository root after adding DEEPGRAM_API_KEY to .env:
    python3 ble_testing

The XIAO must be flashed with oled_mic_streamer.ino. Audio arrives through
the notify characteristic; final transcriptions are written back through the
text characteristic, which the XIAO displays on both OLEDs.
"""

import asyncio
import os
import queue
import threading

from bleak import BleakClient, BleakScanner
from deepgram import DeepgramClient, LiveOptions, LiveTranscriptionEvents
from dotenv import load_dotenv


DEVICE_NAME = "ESP32-Audio"
AUDIO_CHARACTERISTIC_UUID = "12345678-1234-1234-1234-123456789abc"
TEXT_CHARACTERISTIC_UUID = "87654321-4321-4321-4321-cba987654321"
SAMPLE_RATE = 16_000
MAX_OLED_TEXT_BYTES = 192

load_dotenv()
DEEPGRAM_API_KEY = os.getenv("DEEPGRAM_API_KEY")

if not DEEPGRAM_API_KEY:
    raise RuntimeError("DEEPGRAM_API_KEY is missing from .env")


deepgram_connection = None
deepgram_ready = threading.Event()
sentences_to_display: queue.Queue[str] = queue.Queue()


def start_deepgram() -> None:
    """Open Deepgram in a background thread and queue final sentences."""
    global deepgram_connection

    deepgram = DeepgramClient(DEEPGRAM_API_KEY)
    deepgram_connection = deepgram.listen.websocket.v("1")

    def on_open(client, open_event, **kwargs):
        print("[Deepgram] Connected. Listening...")
        deepgram_ready.set()

    def on_transcript(client, result, **kwargs):
        try:
            if not result.is_final:
                return

            transcript = result.channel.alternatives[0].transcript.strip()
            if transcript:
                print(f"[Deepgram] {transcript}")
                sentences_to_display.put(transcript)
        except Exception as error:
            print(f"[Deepgram] Transcript error: {error}")

    def on_error(client, error, **kwargs):
        print(f"[Deepgram] Error: {error}")

    def on_close(client, close_event, **kwargs):
        print("[Deepgram] Connection closed")
        deepgram_ready.clear()

    deepgram_connection.on(LiveTranscriptionEvents.Open, on_open)
    deepgram_connection.on(LiveTranscriptionEvents.Transcript, on_transcript)
    deepgram_connection.on(LiveTranscriptionEvents.Error, on_error)
    deepgram_connection.on(LiveTranscriptionEvents.Close, on_close)

    options = LiveOptions(
        model="nova-3",
        language="en",
        encoding="linear16",
        sample_rate=SAMPLE_RATE,
        channels=1,
        interim_results=True,
        utterance_end_ms=1000,
    )
    deepgram_connection.start(options)


def audio_callback(sender: int, data: bytearray) -> None:
    """Forward each PCM16 BLE notification to Deepgram."""
    if deepgram_ready.is_set() and deepgram_connection is not None:
        deepgram_connection.send(bytes(data))


async def send_pending_sentences(client: BleakClient) -> None:
    """Send queued final transcripts to the XIAO OLED text characteristic."""
    while True:
        try:
            sentence = sentences_to_display.get_nowait()
        except queue.Empty:
            return

        text = sentence.encode("utf-8")[:MAX_OLED_TEXT_BYTES]
        await client.write_gatt_char(TEXT_CHARACTERISTIC_UUID, text, response=False)
        print(f"[OLED] {text.decode('utf-8', errors='replace')}")


async def ble_loop() -> None:
    """Keep reconnecting if the XIAO drops the BLE link."""
    event_loop = asyncio.get_running_loop()

    while True:
        print(f"[BLE] Scanning for {DEVICE_NAME}...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)
        if device is None:
            print("[BLE] XIAO not found. Retrying in 2 seconds...")
            await asyncio.sleep(2)
            continue

        disconnected = asyncio.Event()

        def on_disconnect(client: BleakClient) -> None:
            event_loop.call_soon_threadsafe(disconnected.set)

        try:
            print(f"[BLE] Found {device.address}. Connecting...")
            async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                await client.write_gatt_char(
                    TEXT_CHARACTERISTIC_UUID,
                    b"Laptop BLE connected",
                    response=False,
                )
                print("[OLED] Sent connection test. Waiting one second before audio starts...")
                await asyncio.sleep(1)

                await client.start_notify(AUDIO_CHARACTERISTIC_UUID, audio_callback)
                print("[BLE] Audio connected. Final sentences will be sent to the OLEDs.")

                while client.is_connected and not disconnected.is_set():
                    await send_pending_sentences(client)
                    await asyncio.sleep(0.05)
        except Exception as error:
            print(f"[BLE] Connection error: {error}")

        print("[BLE] Disconnected. Retrying in 2 seconds...")
        await asyncio.sleep(2)


def main() -> None:
    threading.Thread(target=start_deepgram, daemon=True).start()
    print("[Deepgram] Connecting...")
    if not deepgram_ready.wait(timeout=10):
        raise RuntimeError("Deepgram did not connect within 10 seconds")

    try:
        asyncio.run(ble_loop())
    finally:
        if deepgram_connection is not None:
            deepgram_connection.finish()


if __name__ == "__main__":
    main()
