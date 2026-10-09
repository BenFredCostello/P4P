# Real-Time Transcription Glasses

Wearable augmented reality glasses that provide real-time speech-to-text transcription through a heads-up display (HUD), designed to improve communication accessibility for people with hearing impairments.

Developed as a final-year Mechatronics Engineering project at the University of Auckland.

## Key Results

- **End-to-End Latency:** ~600 ms
- **Average Word Error Rate (WER):** <2% under tested conditions

## System Overview

The system integrates embedded audio acquisition, Bluetooth Low Energy (BLE), cloud-based speech recognition, and an optical display into a wearable prototype.

### How It Works

1. An **INMP441 I2S microphone** captures speech at 16 kHz.
2. An **ESP32-S3** streams audio to a companion iOS application over BLE.
3. The iOS application sends audio to **Deepgram** for real-time speech recognition.
4. Transcribed text is sent back to the ESP32-S3 and displayed on an **OLED heads-up display** through an optical reflector.

## Hardware

- Seeed Studio XIAO ESP32-S3
- INMP441 I2S digital microphone
- SSD1306 OLED display
- Optical reflector and wearable glasses frame

## Technologies Used

- **Embedded Firmware:** C/C++
- **Mobile Application:** Swift (iOS)
- **Wireless Communication:** Bluetooth Low Energy (BLE)
- **Speech Recognition:** Deepgram Streaming API
- **Hardware Interfaces:** I2S, I2C
- **Development Tools:** Arduino, Xcode, Python

## Testing and Validation

The prototype was evaluated through controlled speech recognition experiments and user testing.

Performance was assessed using:
- Word Error Rate (WER)
- End-to-end transcription latency
- Speech recognition under different acoustic conditions
- User feedback on comfort, readability, and usability

## Engineering Highlights

- Optimised BLE packet sizes and buffering to reduce audio transmission latency.
- Integrated embedded firmware, an iOS application, and cloud speech recognition into a real-time system.
- Developed a wearable optical display for presenting live transcriptions within the user's field of view.

## Acknowledgements

Developed at the University of Auckland under the supervision of Dr Justine Hui.
