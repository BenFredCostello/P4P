import asyncio
from bleak import BleakScanner, BleakClient
import struct
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque
import threading
import time
import numpy as np
from faster_whisper import WhisperModel

# ── Config ────────────────────────────────────────────────────────────────────
DEVICE_NAME = "ESP32-Audio"
AUDIO_CHARACTERISTIC_UUID = "12345678-1234-1234-1234-123456789abc"
SAMPLE_RATE = 16000          # must match ESP32 SAMPLE_RATE
CHUNK_SECONDS = 2            # transcribe every N seconds of audio

# ── Shared state ──────────────────────────────────────────────────────────────
WINDOW = 1600
plot_buffer = deque([0] * WINDOW, maxlen=WINDOW)

audio_lock = threading.Lock()
audio_accumulator = []

last_print = 0
last_transcribe = time.time()

# ── Load faster-whisper tiny model ────────────────────────────────────────────
print("Loading faster-whisper 'small' model...")
model = WhisperModel("small", device="cpu", compute_type="int8")
print("Whisper ready.")

# ── BLE callback ──────────────────────────────────────────────────────────────
def audio_callback(sender, data):
    global last_print, last_transcribe

    samples = struct.unpack(f'{len(data) // 2}h', data)
    plot_buffer.extend(samples)

    with audio_lock:
        audio_accumulator.extend(samples)

    now = time.time()
    if now - last_print > 0.5:
        print(f"[BLE] Sample values: {samples[:5]}")
        last_print = now

    if now - last_transcribe >= CHUNK_SECONDS:
        last_transcribe = now
        with audio_lock:
            chunk = list(audio_accumulator)
            audio_accumulator.clear()

        threading.Thread(target=transcribe, args=(chunk,), daemon=True).start()


def transcribe(samples_int16):
    if len(samples_int16) < SAMPLE_RATE // 2:
        return

    audio_f32 = np.array(samples_int16, dtype=np.float32) / 32768.0

    segments, _ = model.transcribe(audio_f32, language="en")
    text = " ".join(s.text for s in segments).strip()
    if text:
        print(f"\n[Whisper] {text}\n")

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
        print("Connected. Streaming audio + transcribing...")
        while True:
            await asyncio.sleep(0.1)

def start_ble():
    asyncio.run(ble_loop())

# ── Start BLE in background thread ────────────────────────────────────────────
t = threading.Thread(target=start_ble, daemon=True)
t.start()

# ── Live matplotlib graph (runs on main thread) ────────────────────────────────
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