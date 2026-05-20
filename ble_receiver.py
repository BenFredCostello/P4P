import asyncio
from bleak import BleakScanner, BleakClient
import numpy as np
import struct

DEVICE_NAME = "ESP32-Audio"
AUDIO_CHARACTERISTIC_UUID = "12345678-1234-1234-1234-123456789abc"

audio_buffer = []

def audio_callback(sender, data):
    # data arrives as raw bytes, unpack as int16 samples
    samples = struct.unpack(f'{len(data)//2}h', data)
    audio_buffer.extend(samples)
    print(f"Received {len(samples)} samples, total buffer: {len(audio_buffer)}")

async def main():
    print("Scanning for ESP32-Audio...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME)
    
    if device is None:
        print("Could not find ESP32-Audio. Is it powered on and advertising?")
        return

    print(f"Found device: {device.address}")
    
    async with BleakClient(device) as client:
        print("Connected!")
        await client.start_notify(AUDIO_CHARACTERISTIC_UUID, audio_callback)
        print("Subscribed to audio stream. Listening...")
        
        # just keep running
        while True:
            await asyncio.sleep(1)

asyncio.run(main())