import asyncio
from bleak import BleakScanner, BleakClient
import struct
from collections import deque
import threading
import time
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from deepgram import (
    DeepgramClient,
    LiveTranscriptionEvents,
    LiveOptions,
)

# ── Config ────────────────────────────────────────────────────────────────────
DEVICE_NAME = "ESP32-Audio"
AUDIO_CHARACTERISTIC_UUID = "12345678-1234-1234-1234-123456789abc"
SAMPLE_RATE = 16000
DEEPGRAM_API_KEY = "57490a6b4c360d6736e21b34c716649943e5e0a2"  # replace with your key

# ── Shared state ──────────────────────────────────────────────────────────────
WINDOW = 1600
plot_buffer = deque([0] * WINDOW, maxlen=WINDOW)
last_print = 0

dg_connection = None
dg_ready = threading.Event()

# ── Deepgram setup ────────────────────────────────────────────────────────────
def start_deepgram():
    deepgram = DeepgramClient(DEEPGRAM_API_KEY)
    global dg_connection
    dg_connection = deepgram.listen.websocket.v("1")

    def on_transcript(self, result, **kwargs):
        try:
            transcript = result.channel.alternatives[0].transcript
            if transcript.strip():
                print(f"\n[Deepgram] {transcript}\n")
        except Exception as e:
            print(f"[Deepgram] Error parsing transcript: {e}")

    def on_open(self, open, **kwargs):
        print("[Deepgram] Connection open, streaming...")
        dg_ready.set()

    def on_error(self, error, **kwargs):
        print(f"[Deepgram] Error: {error}")

    def on_close(self, close, **kwargs):
        print("[Deepgram] Connection closed")

    dg_connection.on(LiveTranscriptionEvents.Open, on_open)
    dg_connection.on(LiveTranscriptionEvents.Transcript, on_transcript)
    dg_connection.on(LiveTranscriptionEvents.Error, on_error)
    dg_connection.on(LiveTranscriptionEvents.Close, on_close)

    options = LiveOptions(
        model="nova-2",
        language="en",
        encoding="linear16",
        sample_rate=SAMPLE_RATE,
        channels=1,
        interim_results=True,
        utterance_end_ms=1000,
    )

    dg_connection.start(options)

threading.Thread(target=start_deepgram, daemon=True).start()

# ── BLE callback ──────────────────────────────────────────────────────────────
def audio_callback(sender, data):
    global last_print

    samples = struct.unpack(f'{len(data) // 2}h', data)
    plot_buffer.extend(samples)

    if dg_ready.is_set() and dg_connection is not None:
        dg_connection.send(data)

    now = time.time()
    if now - last_print > 0.5:
        print(f"[BLE] Sample values: {samples[:5]}")
        last_print = now

# ── BLE loop ──────────────────────────────────────────────────────────────────
async def ble_loop():
    print(f"Scanning for '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME)
    if device is None:
        print("Device not found.")
        return
    print(f"Found: {device.address}")
    async with BleakClient(device) as client:
        await client.start_notify(AUDIO_CHARACTERISTIC_UUID, audio_callback)
        print("Connected. Streaming audio...")
        while True:
            await asyncio.sleep(0.1)

def start_ble():
    asyncio.run(ble_loop())

print("Connecting to Deepgram...")
dg_ready.wait(timeout=10)

t = threading.Thread(target=start_ble, daemon=True)
t.start()

# ── Live matplotlib graph ──────────────────────────────────────────────────────
fig, ax = plt.subplots()
line, = ax.plot(list(plot_buffer))
ax.set_ylim(-32768, 32768)
ax.set_title("Mic amplitude")
ax.set_ylabel("Sample value")
ax.set_xlabel("Samples")

def update(frame):
    line.set_ydata(list(plot_buffer))
    return line,

ani = animation.FuncAnimation(fig, update, interval=50, blit=True)
plt.show()