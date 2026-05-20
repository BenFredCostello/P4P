import asyncio
from bleak import BleakScanner, BleakClient
import struct
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque
import threading
import time

DEVICE_NAME = "ESP32-Audio"
AUDIO_CHARACTERISTIC_UUID = "12345678-1234-1234-1234-123456789abc"

WINDOW = 1600  # samples to show on graph at once
plot_buffer = deque([0] * WINDOW, maxlen=WINDOW)
last_print = 0

def audio_callback(sender, data):
    global last_print
    samples = struct.unpack(f'{len(data)//2}h', data)
    plot_buffer.extend(samples)

    # throttle prints to once per second
    now = time.time()
    if now - last_print > 0.5:
        print(f"Sample values: {samples[:5]}")
        last_print = now

async def ble_loop():
    print("Scanning for ESP32-Audio...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME)
    if device is None:
        print("Device not found")
        return
    print(f"Found: {device.address}")
    async with BleakClient(device) as client:
        await client.start_notify(AUDIO_CHARACTERISTIC_UUID, audio_callback)
        print("Connected. Streaming...")
        while True:
            await asyncio.sleep(0.1)

def start_ble():
    asyncio.run(ble_loop())

# run BLE in background thread so matplotlib can run on main thread
t = threading.Thread(target=start_ble, daemon=True)
t.start()

# live graph
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