import asyncio
import os
import queue
import struct
import threading
import time
from collections import deque

import matplotlib.animation as animation
import matplotlib.pyplot as plt
from bleak import BleakClient, BleakScanner
from dotenv import load_dotenv

from deepgram import (
    DeepgramClient,
    DeepgramClientOptions,
    LiveOptions,
    LiveTranscriptionEvents,
)


# ── Config ────────────────────────────────────────────────────────────────────

DEVICE_NAME = "ESP32-Audio"
AUDIO_CHARACTERISTIC_UUID = "12345678-1234-1234-1234-123456789abc"

SAMPLE_RATE = 16000
BYTES_PER_SAMPLE = 2

# 100 ms of 16 kHz, 16-bit mono PCM:
# 16000 samples/s × 2 bytes × 0.1 s = 3200 bytes
DG_BATCH_BYTES = 3200

# ~1.4 s maximum if BLE packets are ~180 bytes each.
AUDIO_QUEUE_MAX_PACKETS = 256

load_dotenv()
DEEPGRAM_API_KEY = os.getenv("DEEPGRAM_API_KEY")

if not DEEPGRAM_API_KEY:
    raise RuntimeError("DEEPGRAM_API_KEY is not set")


# ── Shared state ──────────────────────────────────────────────────────────────

WINDOW = 1600
plot_buffer = deque([0] * WINDOW, maxlen=WINDOW)

audio_queue = queue.Queue(maxsize=AUDIO_QUEUE_MAX_PACKETS)

ble_connected = threading.Event()
dg_ready = threading.Event()
dg_closed = threading.Event()
stop_event = threading.Event()

dg_connection = None

last_print = 0


# ── Statistics ────────────────────────────────────────────────────────────────

stats_lock = threading.Lock()

ble_bytes_received = 0
dg_bytes_sent = 0
dropped_ble_packets = 0


def stats_loop():
    global ble_bytes_received
    global dg_bytes_sent

    last_ble = 0
    last_dg = 0

    while not stop_event.is_set():
        time.sleep(1)

        with stats_lock:
            current_ble = ble_bytes_received
            current_dg = dg_bytes_sent
            dropped = dropped_ble_packets

        ble_rate = current_ble - last_ble
        dg_rate = current_dg - last_dg

        last_ble = current_ble
        last_dg = current_dg

        print(
            f"[STATS] "
            f"BLE={ble_rate / 1000:.1f} kB/s | "
            f"DG={dg_rate / 1000:.1f} kB/s | "
            f"queue={audio_queue.qsize()} | "
            f"dropped={dropped}"
        )


# ── Audio queue helpers ───────────────────────────────────────────────────────

def clear_audio_queue():
    while True:
        try:
            audio_queue.get_nowait()
        except queue.Empty:
            break


def queue_audio(data):
    """
    Called from the BLE notification callback.

    Never wait here. If the queue somehow fills, drop the oldest packet
    rather than blocking BLE and creating more latency.
    """
    global dropped_ble_packets

    try:
        audio_queue.put_nowait(bytes(data))

    except queue.Full:
        try:
            audio_queue.get_nowait()
        except queue.Empty:
            pass

        try:
            audio_queue.put_nowait(bytes(data))
        except queue.Full:
            pass

        with stats_lock:
            dropped_ble_packets += 1


# ── BLE callback ──────────────────────────────────────────────────────────────

def audio_callback(sender, data):
    global last_print
    global ble_bytes_received

    if not data:
        return

    # PCM16 requires an even number of bytes.
    if len(data) % 2 != 0:
        print(f"[BLE] Warning: odd packet size {len(data)}")
        data = data[:-1]

    if not data:
        return

    # IMPORTANT:
    # Do not send to Deepgram from this callback.
    # Queue the bytes and return quickly.
    queue_audio(data)

    with stats_lock:
        ble_bytes_received += len(data)

    # Plotting/debug samples
    samples = struct.unpack(f"<{len(data) // 2}h", data)
    plot_buffer.extend(samples)

    now = time.time()

    if now - last_print >= 0.5:
        print(f"[BLE] Sample values: {samples[:5]}")
        last_print = now


# ── Deepgram connection ───────────────────────────────────────────────────────

def create_deepgram_connection():
    global dg_connection

    dg_ready.clear()
    dg_closed.clear()

    # Your code is using the Deepgram v3/v4-style API.
    # This enables the SDK's KeepAlive handling as additional protection
    # against inactivity timeouts.
    config = DeepgramClientOptions(
        options={
            "keepalive": "true",
        }
    )

    deepgram = DeepgramClient(
        DEEPGRAM_API_KEY,
        config,
    )

    connection = deepgram.listen.websocket.v("1")
    dg_connection = connection

    def on_open(self, open, **kwargs):
        print("[Deepgram] Connection open")
        dg_ready.set()

    def on_transcript(self, result, **kwargs):
        try:
            if not result.is_final:
                return

            transcript = result.channel.alternatives[0].transcript

            if transcript.strip():
                print(f"\n[Deepgram] {transcript}\n")

        except Exception as e:
            print(f"[Deepgram] Transcript error: {e}")

    def on_error(self, error, **kwargs):
        print(f"[Deepgram] Error: {error}")

        dg_ready.clear()
        dg_closed.set()

    def on_close(self, close, **kwargs):
        print("[Deepgram] Connection closed")

        dg_ready.clear()
        dg_closed.set()

    connection.on(
        LiveTranscriptionEvents.Open,
        on_open,
    )

    connection.on(
        LiveTranscriptionEvents.Transcript,
        on_transcript,
    )

    connection.on(
        LiveTranscriptionEvents.Error,
        on_error,
    )

    connection.on(
        LiveTranscriptionEvents.Close,
        on_close,
    )

    options = LiveOptions(
        model="nova-3",
        language="en",
        encoding="linear16",
        sample_rate=SAMPLE_RATE,
        channels=1,
        interim_results=True,
        utterance_end_ms=1000,
    )

    print("[Deepgram] Connecting...")

    started = connection.start(options)

    if started is False:
        raise RuntimeError("Deepgram connection.start() failed")

    if not dg_ready.wait(timeout=5):
        raise RuntimeError("Deepgram did not open within 5 seconds")

    return connection


# ── Deepgram sender/reconnection thread ───────────────────────────────────────

def deepgram_manager():
    global dg_connection
    global dg_bytes_sent

    while not stop_event.is_set():

        # Don't open Deepgram until BLE is actually connected.
        if not ble_connected.wait(timeout=0.5):
            continue

        connection = None

        try:
            # Throw away old/stale audio when starting a new STT session.
            clear_audio_queue()

            connection = create_deepgram_connection()

            print("[Deepgram] Ready for BLE audio")

            batch = bytearray()
            batch_started = None

            while (
                not stop_event.is_set()
                and ble_connected.is_set()
                and dg_ready.is_set()
                and not dg_closed.is_set()
            ):

                try:
                    packet = audio_queue.get(timeout=0.05)

                    if not batch:
                        batch_started = time.monotonic()

                    batch.extend(packet)

                except queue.Empty:
                    pass

                now = time.monotonic()

                batch_old_enough = (
                    batch
                    and batch_started is not None
                    and now - batch_started >= 0.1
                )

                # Send about 100 ms of PCM at a time.
                if len(batch) >= DG_BATCH_BYTES or batch_old_enough:

                    # Keep PCM16 alignment.
                    send_length = len(batch) & ~1

                    if send_length == 0:
                        continue

                    audio = bytes(batch[:send_length])
                    del batch[:send_length]

                    if batch:
                        batch_started = time.monotonic()
                    else:
                        batch_started = None

                    try:
                        connection.send(audio)

                        with stats_lock:
                            dg_bytes_sent += len(audio)

                    except Exception as e:
                        print(f"[Deepgram] send() failed: {e}")

                        dg_ready.clear()
                        dg_closed.set()

                        break

        except Exception as e:
            print(f"[Deepgram] Connection problem: {e}")

        finally:
            dg_ready.clear()
            dg_closed.set()

            if connection is not None:
                try:
                    connection.finish()
                except Exception:
                    pass

            dg_connection = None

            clear_audio_queue()

        if (
            not stop_event.is_set()
            and ble_connected.is_set()
        ):
            print("[Deepgram] Reconnecting...")
            time.sleep(1)


# ── BLE loop ──────────────────────────────────────────────────────────────────

async def ble_loop():

    while not stop_event.is_set():

        try:
            print(f"Scanning for '{DEVICE_NAME}'...")

            device = await BleakScanner.find_device_by_name(
                DEVICE_NAME,
                timeout=10,
            )

            if device is None:
                print("[BLE] Device not found. Retrying...")
                await asyncio.sleep(2)
                continue

            print(f"[BLE] Found: {device.address}")

            disconnected = asyncio.Event()
            event_loop = asyncio.get_running_loop()

            def disconnected_callback(client):
                print("[BLE] Disconnected")

                ble_connected.clear()

                event_loop.call_soon_threadsafe(
                    disconnected.set
                )

            async with BleakClient(
                device,
                disconnected_callback=disconnected_callback,
            ) as client:

                print("[BLE] Connected")

                await client.start_notify(
                    AUDIO_CHARACTERISTIC_UUID,
                    audio_callback,
                )

                ble_connected.set()

                print("[BLE] Streaming audio...")

                await disconnected.wait()

        except Exception as e:
            print(f"[BLE] Error: {e}")

        finally:
            ble_connected.clear()

        if not stop_event.is_set():
            print("[BLE] Reconnecting...")
            await asyncio.sleep(1)


def start_ble():
    asyncio.run(ble_loop())


# ── Start background threads ──────────────────────────────────────────────────

threading.Thread(
    target=deepgram_manager,
    daemon=True,
).start()

threading.Thread(
    target=stats_loop,
    daemon=True,
).start()

threading.Thread(
    target=start_ble,
    daemon=True,
).start()


# ── Live matplotlib graph ─────────────────────────────────────────────────────

fig, ax = plt.subplots()

line, = ax.plot(list(plot_buffer))

ax.set_ylim(-32768, 32768)
ax.set_title("Mic amplitude")
ax.set_ylabel("Sample value")
ax.set_xlabel("Samples")


def update(frame):
    line.set_ydata(list(plot_buffer))
    return line,


ani = animation.FuncAnimation(
    fig,
    update,
    interval=50,
    blit=True,
)

try:
    plt.show()

finally:
    stop_event.set()