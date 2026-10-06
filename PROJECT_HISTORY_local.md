# Real-Time Transcriber Glasses: Project History

**Project:** University of Auckland Part IV Mechatronics Engineering research project  
**Research students:** Tavish Puri and Ben Costello  
**Supervisor:** Dr Justine Hui  
**History updated:** 5 October 2026, New Zealand time  
**Repository inspected:** `P4P_Code`, branch `mic-serial-audio-streaming`  
**Latest recorded commit:** `1aae8a2`, 18 September 2026, `flipped text fixed`

## 1. Purpose and coverage

This document records the development of the real-time transcription glasses, from the initial project scope through component selection, prototyping, software development, integration, and preparation for evaluation. It includes the approaches that were replaced, the problems encountered, and the reasons behind important changes.

The history was reconstructed from the project scope and literature review, the mid-year technical report, local development conversations, Git history, current source files, and recent testing and ethics discussions. It covers the accessible records through 5 October 2026. Exact dates are given where a document, conversation, or commit establishes them; otherwise, work is described by its development stage. At Tavish's instruction, the latest source code is the authority for current implementation behaviour. Older reports and conversations describe earlier stages when they disagree with that code.

Three kinds of evidence are kept distinct:

- **Implemented:** the feature or change is present in source code or a recorded revision.
- **Reported working:** Tavish reported successful behaviour during a physical test, or recorded it in a commit message.
- **Planned:** the activity was discussed or documented, but completed measurements or results were not found.

The prototype has been reported working through the complete transcription path. Descriptions such as "minimal latency" refer to development observations, rather than a measured end-to-end latency result. WER results, objective latency measurements, and completed participant evaluations were not found in the reviewed records.

## 2. Project origin and intended improvements

The project aims to convert nearby speech into text that a wearer can read within their field of view. The intended application is assistive communication for people with hearing impairment, particularly during everyday conversation.

The work builds on previous University of Auckland transcription-glasses projects. Those projects had already demonstrated the glasses concept, so this project was framed around improving the implementation. Tavish identified three main concerns with the earlier designs: wired operation, a bulky or uncomfortable assembly, and eye strain.

The resulting design priorities were:

1. Provide a wireless connection between the glasses and the processing device.
2. Keep the assembly compact and practical to wear.
3. Present captions through an optical path intended to reduce the need to focus on a very close display.

The scope and literature review dated **2 April 2026** described a broader research direction involving BLE audio transmission, a collimated optical display, and directional MEMS microphone capture. It also proposed evaluating transcription accuracy and user experience.

The directional-audio objective remained broader than the implemented baseline. The reviewed firmware captures one microphone channel and sends mono audio. No completed microphone-array processing, beamforming, or measured directional noise-rejection result was found.

### 2.1 Original scope and research objectives

The April scope set out six objectives:

| Objective | Intended work | Position in the reviewed history |
|---|---|---|
| Wireless system architecture | BLE connection between the glasses and an external processing device | Implemented with laptop and iPhone clients; repeatedly tested during development |
| AR optical display | Collimated caption overlay using a miniature display, lenses, and beam splitter | Optical design documented; physical lens-viewing and orientation problems reported |
| Directional audio capture | Investigate MEMS microphone capture and noise rejection | Single-microphone baseline implemented; directional processing not established |
| Transcription accuracy | Evaluate the complete system using WER | Testing approach discussed; results not found |
| User evaluation | Assess legibility, comfort, speed, and usefulness | Questionnaire and participant documents prepared; completed responses not found |
| Design documentation | Record architecture and component decisions | Scope, report, diagrams, presentation materials, and code documentation developed |

The original scope assumed face-to-face conversation at approximately 0.5 to 2 metres. Battery life and miniaturisation were considered, but were not the primary optimisation targets at the prototype stage. These are original planning assumptions, rather than verified operating limits.

## 3. Architecture and component selection

Component selection preceded prototype construction. Tavish explicitly corrected earlier report drafts that suggested the hardware had been selected to reuse an already-developed prototype. The development order was concept, component selection, breadboard testing, and progressively integrated prototypes.

The intended data path was:

```text
Speech
  -> microphone on glasses
  -> ESP32 audio capture and conversion
  -> BLE audio stream
  -> laptop or iPhone client
  -> Deepgram streaming transcription
  -> transcript returned over BLE
  -> ESP32 display rendering
  -> OLED and optical path
  -> wearer
```

The laptop acted as an early development and diagnostic client. The iPhone app became the intended portable bridge between the glasses and the transcription service.

### 3.1 Microcontroller

The selected wearable controller was the **Seeed Studio XIAO ESP32S3**. Its small form factor was a central reason for the choice, alongside BLE connectivity, digital audio support, and the Arduino-ESP32 software ecosystem.

The mid-year report compared it with the Arduino Nano 33 BLE Rev2 and Raspberry Pi Pico 2 W. The comparison focused on board size, processing capability, radio support, and battery integration. The report listed the XIAO footprint as 21 by 17.8 mm and treated its smaller area as useful for glasses-mounted electronics.

Earlier standalone sketches also used a larger ESP32-style pin arrangement. In July, Tavish distinguished an earlier microphone-to-phone board from the XIAO setup that also carried the OLEDs. The history therefore contains more than one hardware configuration, and older GPIO assignments should not be treated as the current wearable wiring.

### 3.2 Microphone

The project used an **INMP441 I2S MEMS microphone**. Its digital output allowed audio to pass directly into the ESP32 I2S peripheral without adding an analogue microphone and ADC signal path.

The working audio format became 16 kHz, mono, signed 16-bit PCM. The microphone supplies audio in 32-bit I2S slots, so firmware converts the captured values before sending them to the transcription client.

Digital gain was adjusted during development to make speech more visible and useful in the captured stream. An early revision added an eight-times multiplier. The later baseline used `MIC_GAIN = 16`, with saturation before conversion to `int16_t`.

These were software scaling changes. They do not establish an improvement in the microphone's underlying acoustic sensitivity or a measured signal-to-noise ratio.

### 3.3 OLED and optical components

The selected display was a **0.42-inch SSD1306 OLED with a 72 by 40 pixel visible area**. Two OLED modules were used in the breadboard setup. Tavish later clarified that the wearable glasses would use **one OLED and one optical path**.

The optical arrangement described in the project documents contains a magnifying lens, a collimating lens, and a half beam splitter. The magnifier enlarges the small display image. The collimating stage is intended to present the caption at a distant apparent focus. The beam splitter combines the display light with the wearer's view of the surroundings.

Reducing eye strain was a design objective. The reviewed records do not contain measurements confirming optical infinity, eye-box dimensions, or a reduction in eye strain relative to the previous prototype.

### 3.4 Mechanical assembly and power

The mechanical approach used a custom 3D-printed mount attached to a store-bought pair of glasses. The mount was intended to hold the display, optical components, and supporting electronics in a compact arrangement.

The mid-year report described the printed mount and corresponding engineering drawing. The conference planning assigned the optics and 3D-printing discussion to Ben, and the pipeline, BLE, and app discussion to Tavish. That supports a broad division of presentation responsibilities, but does not establish authorship of every physical component.

Battery operation was part of the wearable design, with a compact lithium-polymer battery and the XIAO charging arrangement discussed in the report. No measured battery runtime, complete power budget, or final assembled-device mass was found.

## 4. First component tests and laptop audio reception: May 2026

### 4.1 Basic OLED operation

The repository began on **14 May 2026**. The first display revision used the Adafruit GFX and SSD1306 libraries to initialise the display and show fixed text.

The early test used `Wire.begin(21, 22)`. Later XIAO integration used GPIO5 and GPIO6 instead. Those files record different stages of hardware development, rather than one consistent wiring map.

The initial test established a simple milestone: communicate with the OLED and render known text before attempting live captions.

### 4.2 Microphone response

On **20 May**, microphone testing recorded peak sample values and observed them increasing when someone spoke into the microphone. The early test sketch used I2S WS on GPIO4, SCK on GPIO5, and SD on GPIO6.

The test demonstrated a response to sound, but did not yet establish correct scaling, correct stereo-slot interpretation, or transcription accuracy. Those became separate debugging tasks later.

### 4.3 BLE audio export and waveform plotting

Also on **20 May**, firmware was added to export microphone data over BLE. The peripheral advertised as `ESP32-Audio`, and the Python receiver used Bleak to discover it and subscribe to audio notifications.

The receiver decoded the PCM samples and displayed a live waveform using Matplotlib. The commit message recorded that sound was being received from the ESP and could be plotted.

The waveform became an important diagnostic tool. It allowed audio reception to be checked independently of whether the speech-recognition service returned text.

## 5. Local Whisper experiments and the move to Deepgram: May 2026

### 5.1 Initial local transcription

On **26 May**, the laptop receiver was extended to run local Whisper transcription. The first recorded version used the `base` model, accumulated four seconds of audio, and performed transcription in a separate thread.

The receiver converted the signed PCM16 samples to normalised floating-point audio before passing them to the model. BLE reception and waveform plotting continued alongside transcription.

A subsequent revision used `faster-whisper` with the `small` model, CPU execution, and `int8` computation. The audio chunk interval was reduced to two seconds.

These experiments established a local speech-to-text path. Their chunked workflow also introduced a delay before each transcription could begin, followed by the processing time for that chunk. The project needed captions to appear during conversation, which led to the next approach.

### 5.2 Streaming transcription

Later on **26 May**, the receiver moved to Deepgram. The commit message described it as "way faster transcription". Another revision that day selected the `nova-3` model.

Deepgram was used as an external streaming speech-to-text service. The client sends audio over a persistent WebSocket and receives interim and final transcript events. This suited the project's emphasis on responsive captions without placing the recognition model on the glasses.

The main configuration became:

| Setting | Recorded configuration |
|---|---|
| Model | `nova-3` in the current laptop and phone implementations |
| Encoding | `linear16` |
| Sample rate | 16,000 Hz |
| Channels | One |
| Formatting | Smart formatting enabled |
| Live hypotheses | Interim results enabled in the later clients |
| Utterance-end setting | `utterance_end_ms=1000` in the later full clients |

Some early explanatory text referred to `nova-2`; the Git record and current source establish the later `nova-3` configuration. No controlled benchmark comparing Whisper and Deepgram WER or end-to-end latency was found. The service selection was based on the development experience and the need for streaming responses.

### 5.3 Credential configuration

The Python receiver was changed to load its API credential from a `.env` file, with ignore rules added to keep that configuration out of normal commits.

The iPhone app later retained a hardcoded credential in source. A July comparison found that the laptop and phone credentials did not match, which was a possible explanation for phone transcription failure at that stage. The reviewed history does not establish that credential rotation or secure phone configuration was completed. No credential values are included in this history.

## 6. Native iPhone application: July 2026

### 6.1 Initial app and first working phone path

The SwiftUI project, **TranscriberGlasses**, was created on **8 July**. Its Bluetooth manager used CoreBluetooth for the ESP connection and `URLSessionWebSocketTask` for the Deepgram connection.

The first app revision was recorded as not working on the phone. By **11 July**, a commit reported that the phone transcriber was working with one microphone.

The app provided a portable alternative to the laptop receiver. It discovered the glasses, received BLE audio, forwarded it to Deepgram, and displayed the returned transcript.

The current interface includes a Bluetooth status panel and a transcript panel. It distinguishes scanning, connected, and disconnected states, and shows whether the displayed text is live or final.

### 6.2 Interim captions and repeated words

On **12 July**, a revision enabled interim text and recorded much lower perceived latency. The important change was allowing the user to see a developing transcript before a final result arrived.

Earlier handling of interim results had produced repeated words when revised hypotheses were appended as new text. The adopted phone behaviour separated the live hypothesis from the saved transcript:

- Interim text replaces the previous interim hypothesis.
- Final text is appended to the transcript history.
- The interim field is cleared when a final result is accepted.

The app can therefore show developing speech without appending every partial prediction. Current source still follows this separation.

## 7. Sentence formatting and integrated XIAO prototype: July 2026

### 7.1 OLED sentence renderer

On **15 July**, a standalone display sketch was added to render a complete sentence across several lines. It treated the physical 72 by 40 panel as a window within the SSD1306 controller's 128 by 64 pixel memory.

The renderer used a visible-region offset of `x = 28`, `y = 24`. Its initial layout allowed 12 characters per line across four lines, with word wrapping and hyphenation where a word would not fit.

The standalone test dropped text beyond the fourth line. It established the layout behaviour before transcript delivery and display updates were fully integrated.

### 7.2 Breadboard integration

By **22 July**, the XIAO audio-and-display pipeline was recorded in the repository. The breadboard arrangement made the controller, microphone, and two test OLEDs accessible for wiring checks and isolated tests.

The integrated work introduced separate microphone and transport files. The development sequence tested the display and microphone independently, combined them, and added the wireless client.

An important display fix was explicitly starting I2C with `Wire.begin(5, 6)` for the XIAO. Relying on default pins had left the displays blank.

### 7.3 USB serial development path

A USB serial bridge was also developed to carry audio to the laptop and return transcribed text. Its framed protocol allowed binary audio, text, and logs to share one serial connection:

```text
0xAA | 0x55 | TYPE | LENGTH_LOW | LENGTH_HIGH | PAYLOAD | XOR_CHECKSUM
```

| Type | Direction | Payload |
|---|---|---|
| `0x01` | Device to host | PCM16 mono audio |
| `0x02` | Host to device | UTF-8 sentence |
| `0x03` | Device to host | Debug log text |

The parser could discard a damaged packet and resynchronise on a later header. The Python bridge printed device logs and sent final Deepgram results back to the display.

This path was useful for separating microphone and display behaviour from BLE problems. It is a historical implementation: the serial transport source was removed during the September refactor, although the Python bridge and protocol files remain. The existing `xiao_audio_display/README.md` still describes this older serial architecture.

## 8. Bidirectional BLE integration and reliability work: July 2026

### 8.1 Audio and text characteristics

The full BLE path required audio to travel outward and captions to return to the glasses. The project used a notify characteristic for PCM audio and a writable characteristic for transcript text.

| Item | Value |
|---|---|
| Peripheral name | `ESP32-Audio` |
| Service UUID | `12345678-1234-1234-1234-123456789abc` |
| Audio characteristic UUID | `12345678-1234-1234-1234-123456789abc` |
| Text characteristic UUID | `87654321-4321-4321-4321-cba987654321` |

The service and audio characteristic share a UUID in this implementation. That was discussed as a design issue, but was not proven to cause the disconnections.

### 8.2 Connection failures after integration

Adding BLE to the combined XIAO prototype produced several regressions. Reported symptoms included blank OLEDs, successful phone connection followed by a timeout, and repeated reconnection cycles.

Some phone logs showed service discovery and audio notification subscription succeeding before the connection failed. Other tests failed earlier. The exact failure stage mattered because it separated discovery problems from problems arising during audio streaming or transcript return.

Deepgram errors such as "cancelled" or "Socket is not connected" often followed a BLE disconnect. Those messages could reflect cleanup of the transcription connection, rather than the original cause of the link failure.

### 8.3 Packet size and audio rate

The audio format requires:

```text
16,000 samples/s × 2 bytes/sample × 1 channel = 32,000 bytes/s
```

The phone connection was later confirmed to negotiate an MTU of 185 bytes. Subtracting the three-byte ATT header leaves 182 bytes for a notification payload. The firmware used a maximum of **180 bytes** so each packet contained a whole number of PCM16 samples.

At that payload size:

```text
180 bytes / 2 bytes per sample = 90 samples
90 samples / 16,000 samples/s = 5.625 ms of audio
```

The early 100-sample block produced 200 bytes, so transport code was changed to divide audio into packets that fit the negotiated payload. Packet sizing was one part of the reliability work; it was not established as the sole cause of every timeout.

### 8.4 Ring buffering and asynchronous notification completion

An intermediate firmware design used an audio ring buffer, nonblocking capture, and paced notifications. Its flow-control logic was revised after a race was identified between requesting a notification and receiving the later transmit-status callback.

The faulty approach checked the notification result immediately after calling `notify()`. That could classify a packet as failed before its asynchronous completion arrived. The resulting retries retained already-sent audio in the ring and could create backlog or dropped data.

The revised historical design kept one notification pending, waited for completion, and removed audio from the ring only after successful transmission. BLE callbacks recorded state, and the main loop handled the ring updates. Tavish subsequently reported that this version was working.

The Arduino-ESP32 core and BLE backend also mattered during this work. A recorded build under core 3.3.8 used NimBLE, requiring care with backend-specific descriptors and callbacks. That successful build belongs to the firmware revision used at the time.

The current modular transport has since been simplified. It directly calls `notify()` for each audio packet and does not contain that earlier ring-buffer completion scheme. The historical fix should therefore not be described as an active feature of the latest transport.

### 8.5 Laptop bridge as an integration reference

The separate `ble_testing.py` client was created to receive BLE audio, transcribe it, and send captions back to the ESP. It allowed the full return path to be tested independently of the iPhone app.

An initial failure came from Deepgram callback signatures that did not accept the client argument supplied by the installed SDK. Correcting those signatures allowed the connection to start.

The laptop bridge used text writes without response. Successful laptop transcription and OLED updates provided a reference against which phone behaviour could be compared. Several July tests were reported working after the firmware and I2C changes.

### 8.6 Phone transcript return

The phone sometimes displayed transcripts successfully without updating the OLEDs. The text write path was changed to use `.withoutResponse`, matching the working laptop bridge. Tavish reported that the OLED return path then worked.

Earlier phone versions also experimented with sending interim captions to the glasses, with writes limited to roughly one every 300 ms. These changes were intended to support long sentences without waiting for finalisation.

Successful tests were followed by further regressions, including renewed phone transcription failures while the laptop still worked. The project progressed through working configurations and later reliability problems, rather than reaching one permanent fix in July.

### 8.7 Board selection and upload problems

The XIAO required the correct Arduino board selection. A recorded upload failed with `Failed to connect to ESP32-S3: No serial data received`; bootloader entry and the board configuration were investigated, and Tavish later reported that uploading worked.

Tests also involved more than one peripheral named `ESP32-Audio`. Identifying the active board was necessary before interpreting connection behaviour.

The separate classic ESP32 upload problem discussed on 15 September was explicitly identified by Tavish as independent of this project. It is excluded from this project's hardware failure history.

## 9. Mid-year documentation, presentation, and display concept

### 9.1 Technical report

The mid-year technical report brought together component selection, breadboard testing, audio capture, BLE packetisation, the laptop and phone clients, display formatting, optics, and the mount.

Tavish revised the document so the explanation followed the actual development order. The report also added an explanation of Deepgram and the reason for moving away from the initial Whisper approach.

Further revisions clarified the single-OLED wearable design and expanded the optical explanation. Report figures covered the architecture, breadboard setup, packet timing, mobile pipeline, mount, and optical path. A LaTeX/Overleaf version was prepared to fit the required four-to-six-page technical-report format.

The report's testing section described future measurements. Its packet counters, firmware buffer sizes, and proposed test counts belong to that stage of development and should not automatically be carried forward as current settings or completed experiments.

### 9.2 Mid-year conference preparation

In late July, the team prepared a ten-minute conference presentation. Its central story was the move toward a wireless, comfortable device with lower eye-strain potential.

The proposed split placed the software pipeline, BLE connection, iPhone app, and testing discussion with Tavish. Ben's sections covered optics, the physical mount, and future functionality. Tavish reported that the iPhone app was working during this preparation.

The records establish preparation of the presentation and scripts. They do not independently establish the delivery date or audience feedback.

### 9.3 View-through-glasses simulation

On **28 July**, a first-person simulation was requested to show the glasses frame and captions reflected into the field of view. Tavish asked for a smaller caption region near the bottom of the view, then adjusted it upward so it remained inside the frame.

The simulation helped communicate the intended display position and visual footprint. It was a concept visual, rather than a measurement of the physical optical system.

## 10. Continued debugging: August 2026

### 10.1 Phone-specific disconnects

On **10 August**, Tavish reported that the phone connection repeatedly dropped and returned, while the laptop script stayed connected. Similar behaviour appeared again later in the month.

The comparison shifted attention toward differences between clients, including CoreBluetooth callback handling, network forwarding, and transcript writes. It did not prove that the ESP firmware was correct in every respect, or identify one confirmed phone-side root cause.

Slow dummy-packet tests were discussed to reduce the audio workload and isolate basic BLE behaviour. On 24 August, Tavish reported that the phone still disconnected in dummy mode, while `ble_receiver.py` remained connected with real audio. That evidence weakened a simple explanation based only on the raw PCM data rate.

### 10.2 Audio responsiveness and gain

Microphone sensitivity was revisited on **13 August** and **24 August**. Tavish also reported a delayed waveform after reflashing, occasional transcription, and missing interim results.

The waveform and transcript output served different purposes. A responsive waveform with delayed text pointed toward recognition or caption handling. A delayed waveform suggested delay earlier in capture, transport, or receiver processing.

Some experimental edits were reverted when they did not improve the observed behaviour. No controlled gain-versus-WER study was recorded.

### 10.3 Development environment and display issues

The August work also encountered host-side build problems:

- The Arduino cache could not write files because the disk was full.
- `Adafruit_GFX.h` was missing during compilation.
- An `ESP32_BLE_Arduino` library path produced an `esp_gap_ble_api.h` error.
- Arduino dependency and board-package configuration required investigation.

These failures interrupted building and reflashing. They were development-environment problems, separate from the device's runtime behaviour.

Tavish also reported changes in measured SDA/SCL voltage when the display stopped working, despite checking the ground connection. The reviewed records do not establish a final electrical diagnosis for that observation.

## 11. Simplification and audio corrections: September 2026

### 11.1 Delay diagnosis and a smaller test scope

On **15 September**, the receiver was reviewed because microphone response appeared slower than a previous version. The code review identified final-only transcript handling, synchronous network work in the BLE callback, and firmware buffering as possible contributors at different points in the path.

On **16 September**, Tavish requested a bare-bones milestone: receive BLE audio and print transcription, with other features removed. The receiver was simplified to scan, connect, forward PCM, and print live and final text.

A subsequent laptop run failed during CoreBluetooth service discovery with `BleakError: disconnected`. The failure occurred before audio notification subscription. Deepgram disconnect messages followed cleanup of the test.

This was a BLE setup failure in that run. It did not establish a problem with Deepgram transcription or prove a particular firmware timing hypothesis.

### 11.2 Correct I2S stereo-slot interpretation

By **17 September**, Tavish supplied a working-baseline account identifying a major audio correction. Capturing both I2S slots showed that slot 0 was unused and the microphone signal was present in slot 1.

The mono extraction became:

```cpp
sample = rawSamples[i * 2 + 1] >> 16;
sample *= MIC_GAIN;
sample = constrain(sample, -32768, 32767);
```

Before the slot correction, values such as 536870912 and 1610612735 had appeared. Afterward, Tavish reported silence around approximately ±50 to ±200 and speech rising into the thousands or tens of thousands.

Those observations supported more sensible capture and scaling. They were not calibrated sound-pressure measurements or a final characterisation of microphone noise.

### 11.3 Removing duplicate timing delays

The later baseline used `BUFFER_SAMPLES = 90`, giving 180 bytes of PCM16 per microphone read. At the negotiated payload size, that fit into one notification.

An additional delay after notification transmission was removed because the blocking I2S read already paced acquisition. Adding another delay reduced how quickly the firmware could deliver the continuous stream.

The current microphone code still uses a blocking `i2s_read(..., portMAX_DELAY)`. This differs from the earlier nonblocking ring-buffer implementation.

### 11.4 Separating BLE reception from Deepgram forwarding

The most important host-side architecture change was decoupling the BLE callback from WebSocket work. Instead of sending audio to Deepgram inside the notification callback, the receiver queues it and returns quickly.

The Python worker batches approximately 100 ms of PCM, forwards it, and handles WebSocket errors. It clears readiness when the connection closes and reconnects the transcription service independently of BLE reception. Stale queued audio is discarded during recovery.

The iPhone app adopted a similar separation using dedicated dispatch queues, timed audio flushing, bounded buffering, and independent Deepgram recovery. It also guards against callbacks from a socket that has already been replaced.

The intended recovery behaviour became:

```text
Deepgram connection fails
  -> clear transcription readiness
  -> discard stale audio
  -> reconnect Deepgram
  -> continue with fresh audio while retaining BLE where possible
```

A commit early on **17 September** recorded that transcription was working well, especially on the phone, with minimal perceived latency after these architecture changes.

### 11.5 Throughput observations

The Python receiver added counters for received BLE bytes, forwarded Deepgram bytes, queue depth, and locally dropped packets. Tavish supplied examples such as:

```text
[STATS] BLE=20.9 kB/s | DG=22.3 kB/s | queue=0 | dropped=0
[STATS] BLE=19.8 kB/s | DG=19.8 kB/s | queue=0 | dropped=0
```

These examples showed no queue buildup or local queue drops during those reporting intervals. They did not demonstrate delivery of the full theoretical 32 kB/s microphone stream, or rule out losses before the receiver callback.

Tavish preferred to prioritise working transcription and low delay over further throughput optimisation unless missed speech or degraded transcription became apparent. The lower observed byte rate remained a development observation to revisit during evaluation.

## 12. Modular firmware and final display integration: 17 September 2026

### 12.1 Refactor into separate responsibilities

On **17 September**, the ESP firmware was reorganised into microphone, BLE transport, display, and main-sketch modules:

| File | Responsibility |
|---|---|
| `oled_mic_streamer.ino` | Startup, main loop, audio capture calls, and sentence delivery |
| `mic.cpp` / `mic.h` | I2S setup, slot extraction, gain, clipping, and PCM16 output |
| `transport_ble.cpp` / `transport.h` | Advertising, connection state, audio notifications, and incoming text |
| `display.cpp` / `display.h` | OLED setup, wrapping, centring, and orientation |

The previous serial transport and firmware protocol header were removed during the refactor. The retained Python serial files and older README therefore describe a previous implementation.

Two later commits that day recorded "both paths working decently well" and "full pipeline working, minimal latency". These are useful functional milestones from the developer's tests, rather than formal performance results.

### 12.2 Text return and diagnostic boundaries

Further phone debugging distinguished three events: the phone requesting a write, the ESP receiving the text, and the OLED physically updating.

The phone write path was changed to require a characteristic supporting `.writeWithoutResponse`. It limits the text to the smaller of 180 bytes and the negotiated maximum write length. It also avoids ending a truncated payload partway through a UTF-8 character.

A queued write without response is not a delivery acknowledgement. The ESP's `[TRANSCRIPT]` output provided stronger evidence that the return path had reached the device.

The transport callback copies received text into a protected pending buffer. `transportPoll()` passes that text to the sentence handler from the main loop. The handler prints `[TRANSCRIPT]` and calls the display renderer, keeping I2C work outside the BLE write callback.

### 12.3 Display positioning and a frozen-image problem

Tavish reported that the display was working and requested a narrower text block concentrated near its centre. The layout was reduced from 12 to **10 characters per line**, with each line centred inside the visible window.

A later test showed new `[TRANSCRIPT]` lines in Serial while the OLED remained frozen on older text. The display reportedly retained its image even after uploading and resetting.

Display diagnostics were added to check for an I2C response at `0x3C` during startup and before drawing. A full power removal was discussed to distinguish retained display pixels from a successful redraw. The conversation did not record a conclusive electrical cause for the frozen-image behaviour.

### 12.4 GPIO-powered microphone and pin reassignment

Tavish requested a synthetic microphone supply by driving **D9 high**. The initial implementation used GPIO8 for microphone power and GPIO3 for microphone data.

The later commit `3264c26`, titled `mic pins updated`, swapped those numeric assignments. The current modular `mic.cpp` contains:

| Signal | Current numeric value | Earlier documented XIAO arrangement |
|---|---:|---|
| I2S SCK | GPIO1 | D0 / GPIO1 |
| I2S WS | GPIO2 | D1 / GPIO2 |
| I2S SD | GPIO8 | D2 / GPIO3 |
| GPIO-driven microphone supply | GPIO3 | D9 / GPIO8 in the initial supply change |
| OLED SDA | GPIO5 | D4 / GPIO5 |
| OLED SCL | GPIO6 | D5 / GPIO6 |

The current comments and startup message still refer to the earlier microphone assignments. The standalone `mic_test.ino` also retains SD on GPIO3. The source records therefore establish a pin reassignment, but do not establish the exact wiring of the currently flashed physical device.

The GPIO supply was a prototype arrangement. Electrical performance and suitability as a microphone power source were not verified in the reviewed records.

## 13. Lens-viewed text orientation: 18 September 2026

On **18 September**, Tavish reported that letters appeared reversed or flipped through the lens. He also requested that the captions appear slightly lower.

The first change used an SSD1306 segment-remap command. Physical feedback showed that this did not give the desired view: the text appeared upside down, the word order was wrong, and horizontal flipping remained.

The segment-remap experiment was removed. The final source instead draws the text, vertically flips the pixels within the visible 72 by 40 region, and sends that image to the OLED. A `TEXT_Y_OFFSET = 4` was also added to the pre-flip text placement.

The latest commit is named `flipped text fixed`. The recorded assistant checks for the final patch were source/diff checks; a separate final lens-test confirmation was not found in that conversation. The implementation is present, but the commit title alone should not be used as proof of optical correctness for every viewing arrangement.

## 14. User evaluation, risk assessment, and ethics preparation

### 14.1 Questionnaire development

On **3 August**, Tavish requested a user-testing questionnaire. Earlier report planning had proposed numeric ratings out of ten for comfort and related usability measures, with written feedback.

The available Qualtrics survey printout is dated **10 September 2026** and uses five-point responses. It covers:

- Comfort when wearing the glasses.
- Readability of the displayed text.
- Whether caption position allows a clear view of the surroundings.
- Suitability of text size.
- Whether captions appear quickly enough to follow the speaker.
- Perceived transcription accuracy and help with following conversation.
- Distraction from the display.
- Comfort wearing the device in public.
- Noticeable eye strain.

Open-ended questions ask what users liked, what was difficult or distracting, how text size and position could improve, what speech was missed, and which change would help most.

The survey is evidence of a prepared evaluation instrument. No completed response dataset or resulting statistical analysis was found.

### 14.2 Participant information and consent

Participant information and consent documents were prepared for the **Real-time Transcription Glasses User Study**. They identify Tavish and Ben as the student researchers and Dr Justine Hui as the principal investigator.

The participant information sheet describes a demonstration, fitting the glasses, and a structured trial of approximately 15 to 30 minutes. It also describes ratings and written feedback about comfort, fit, readability, and distraction.

That saved version mentions possible interviews. Tavish explicitly clarified on **5 October** that interviews will not be conducted, and that the interview statement was being removed. The current history follows that clarification rather than the older form wording.

### 14.3 Risk assessment

The available final risk-assessment document records an assessment date of **23 September**, with dated entries for Tavish and Justine on 23 and 25 September respectively.

It covers laboratory work, tools, soldering, electronics, 3D printing, and the particular risks of testing glasses near the face. The glasses-related items include visual discomfort, bright light near the eye, electronics near the head, and handling objects close to the eyes.

The documented controls include short testing periods, checking the display before wearing it, avoiding direct contact between electronics and the body, and careful handling. The assessment is a project preparation record, rather than proof that every proposed test has been completed.

### 14.4 Conditional approval correspondence

On **5 October**, Tavish reported receiving ethics correspondence requiring completion of section **11.A.1**, describing changes made in response to conditional approval.

He had added the changes made by himself and Ben, and intended the supervisor to add any further changes before resubmission. The reviewed conversation does not establish that the application had been resubmitted or that final approval had been granted.

## 15. Quantitative testing discussions: 5 October 2026

### 15.1 WER

The latest discussion returned to a repeated-script WER test using **two speakers**. Tavish indicated that he could use an approach similar to the previous project.

The discussed method compares a saved final transcript with a fixed reference script:

```text
WER (%) = (substitutions + deletions + insertions) / reference words × 100
```

The conversation proposed ten readings per speaker, giving twenty WER samples. It also suggested a short conversational passage used consistently across trials. These counts and script lengths were advice discussed in the chat, rather than an established completed protocol.

Tavish asked whether the script could be arbitrary. The resulting discussion emphasised justifying its vocabulary and sentence structure against the intended conversational use. A final script and completed WER measurements were not found.

### 15.2 Objective caption latency

Tavish clarified that the glasses update as the person speaks. The discussion therefore moved toward measuring the delay between a spoken word and its first appearance on the physical display, rather than measuring only sentence completion.

The proposed definition was:

```text
Word display latency = time word first appears on OLED - time spoken word ends
```

The chat suggested recording speech and the display together, using audio to identify the target word and video to identify its appearance. A possible design used short sentences, selected target words, two speakers, and repeated trials. Sixty observations and video at 60 frames per second were suggested, but were not confirmed as the final adopted method.

First appearance and final stabilisation were also distinguished. An interim caption can appear quickly and still be revised later, so those events measure different behaviour.

No numerical latency result was supplied in the reviewed discussion. The previous projects' measurements and results discussed in that chat belong to those projects and are not results for this prototype.

### 15.3 Current caption-return behaviour

The latest source establishes that interim text updates the phone interface only. In `BluetoothManager.swift`, the final-result branch calls `sendFinalTranscriptToESP()`, while the interim-result branch only updates `interimTranscript`. The current laptop-to-OLED bridge, `ble_testing.py`, also ignores non-final transcript events.

Earlier phone revisions did send interim text to the OLED, but that is historical behaviour. Under the latest implementation, the OLED receives final transcript segments rather than interim hypotheses. Final segments can still arrive during an ongoing conversation; they are not the same as provisional word-by-word captions. Any latency test must describe this final-segment behaviour accurately and record the software revision used.

Tavish confirmed that sentences appear on the physical display while he is talking. That observation is consistent with the current implementation: Deepgram's `is_final` flag means a segment has been finalised, not that the speaker has finished talking. Its separate `speech_final` flag indicates a detected endpoint. The app checks `is_final` and sends each finalised segment immediately, so the OLED can update during continuing speech without receiving interim hypotheses. See [Deepgram's transcript delivery documentation](https://developers.deepgram.com/docs/understand-endpointing-interim-results/).

### 15.4 Display update frequency and exact return path

There is no fixed display-update timer or caption-write throttle in the current phone code. Each non-empty Deepgram result with `is_final = true` triggers one attempted BLE text write, provided the ESP is connected and the writable characteristic is available. Interim results update the phone only. The app does not wait for `speech_final` or an `UtteranceEnd` event before sending a finalised segment.

The 100 ms timer controls audio forwarding to Deepgram, not caption delivery to the OLED. Similarly, `utterance_end_ms=1000` configures a separate gap-detection event; it does not mean captions are sent once per second. The actual interval between display updates depends on when Deepgram returns finalised segments and when those writes reach the ESP. No measured update interval is established by the code alone.

Deepgram's [November 2023 support discussion](https://github.com/orgs/deepgram/discussions/409) describes final results arriving every approximately three to five seconds, with endpointing able to finalise results after detected pauses. That is a service-side cadence described in an older support record, not a display timer implemented by this project or a verified timing guarantee for its current Nova-3 connection. It explains how a long sentence can generate several finalised updates. Actual intervals for this prototype still require measurement.

Each BLE write contains the new segment rather than the complete accumulated phone transcript. The ESP callback stores the latest received segment, and the next main-loop `transportPoll()` call passes it to `displayShowSentence()`. The renderer clears the previous image and draws that segment across up to four centred lines of ten characters. Text beyond the available lines is not displayed. If several writes arrive before the main loop consumes the pending text, the newest segment replaces the older pending segment.

## 16. Current implementation snapshot

The following snapshot describes the latest source inspected on 5 October and is the reference for the project's current implementation.

| Area | Current source behaviour |
|---|---|
| Embedded capture | 16 kHz I2S; both slots captured; slot 1 extracted; gain 16; PCM16 saturation |
| Audio block | 90 mono samples, normally 180 bytes |
| Firmware transport | MTU-aware audio splitting, maximum 180-byte payload, direct notifications |
| Connection recovery | Firmware schedules advertising restart after a 500 ms delay |
| Text reception | Latest incoming text stored in a protected pending buffer and processed from the main loop |
| OLED | Address `0x3C`, I2C GPIO5/GPIO6, 128×64 controller buffer, 72×40 visible window |
| Text layout | Four centred lines, ten characters per line, wrapping and hyphenation |
| Orientation | Vertical pixel flip of the visible region after text drawing |
| Laptop receiver | BLE callback queues PCM; worker batches and reconnects Deepgram; waveform and statistics available |
| Python queue | Maximum 256 packets; oldest queued packet dropped on overflow |
| Phone audio | Dedicated queues; 100 ms flushing; 32,000-byte local audio-buffer limit |
| Phone recovery | Separate Deepgram reconnect handling, stale-socket guards, and keepalive timer |
| Phone captions | Interim text on phone only; final text returned to ESP using writes without response |
| Laptop-to-OLED captions | `ble_testing.py` returns final results only |
| Transcript write size | Limited to negotiated write length and 180 bytes; truncated safely at a UTF-8 boundary |

The following distinctions prevent older documentation from being mistaken for the latest implementation:

- The earlier firmware ring buffer and transmit-completion handling are not present in the current modular transport.
- The report's earlier 16,000-byte phone buffer has been replaced by a 32,000-byte limit in current source.
- The Python serial bridge remains, but its matching embedded transport files were removed.
- The old microphone pin comments and README disagree with the later numeric pin assignments.
- The earlier two-display bench arrangement is not the intended one-display wearable design.
- Current phone and laptop-to-OLED clients return final captions only; interim OLED return belongs to earlier versions.

## 17. Evidence of progress and remaining work

### 17.1 Established development milestones

The reviewed history supports the following milestones:

1. Selected the controller, microphone, display, and optical approach before prototyping.
2. Demonstrated fixed OLED text and microphone response to speech.
3. Exported microphone samples over BLE and plotted them on a laptop.
4. Implemented local Whisper and faster-whisper transcription experiments.
5. Moved to Deepgram streaming transcription for more responsive development behaviour.
6. Developed a native iPhone app and reported successful phone transcription.
7. Added interim caption handling to the phone interface.
8. Developed sentence wrapping and the SSD1306 visible-window renderer.
9. Built serial and BLE transcript-return paths to the OLED.
10. Investigated BLE timeouts, packet sizing, asynchronous notification status, and callback workload.
11. Corrected I2S slot interpretation and added bounded PCM gain.
12. Separated BLE callbacks from Deepgram forwarding and recovery.
13. Refactored the firmware into microphone, transport, display, and main modules.
14. Integrated the caption path with the display and reported working full-pipeline configurations.
15. Adjusted caption width, position, and lens-viewed orientation.
16. Prepared the technical report, presentation material, concept simulation, survey, participant documents, and risk assessment.
17. Progressed ethics revisions and discussed objective WER and latency testing.

### 17.2 Work not yet established by the reviewed records

| Item | Recorded position |
|---|---|
| Final WER dataset | Method discussed; completed values not found |
| Measured word-to-OLED latency | Definition discussed; recordings and measurements not found |
| Completed user study | Survey and forms prepared; responses not found |
| Final ethics approval | Conditional-approval response being prepared on 5 October |
| Directional microphone processing | Original objective; implementation and results not established |
| Long-duration reliability | Multiple working tests reported; a controlled endurance dataset not found |
| Battery runtime and power consumption | Battery arrangement discussed; measured results not found |
| Optical performance and eye-strain improvement | Design intent and viewing tests recorded; quantified improvement not found |
| Physical test configuration | Latest source defines the implementation; flashed revision and physical wiring still need recording with measurements |

The immediate evaluation work is to record a reproducible working configuration, complete the ethics process for participant testing, and collect objective WER and caption-latency measurements. The survey can then provide separate evidence about comfort, readability, distraction, and usefulness.

## 18. Dated development index

Dates below use the New Zealand dates recorded in the Git history or source documents. Conversation dates identify the available discussion, rather than an assumed date for every physical task.

| Date | Record | Milestone |
|---|---|---|
| 2 April | Scope and literature review | BLE, optical overlay, directional capture, accuracy, and user-evaluation objectives documented |
| 14 May | `33378e6`, `e114c17` | Repository started; basic SSD1306 display test added |
| 20 May | `408a8f7` | Microphone response to speech recorded |
| 20 May | `24d78ad`, `d4f6880`, `59bf067` | BLE receiver, embedded audio export, and laptop waveform reception |
| 26 May | `2f50eaa` | Local Whisper transcription with four-second chunks |
| 26 May | `47f0451` | Faster-whisper small model and two-second chunks |
| 26 May | `601685b`, `e5741f2` | Deepgram streaming introduced; nova-3 selected |
| 26 May | `c2400c3`, `ee30a2d`, `22b7564` | Environment-based Python credential setup, ignore rules, and eight-times audio gain |
| 8 July | `2b2933b`, `d6c5518`, `cdef602` | Existing code consolidated; native phone app created; initial phone path still failing |
| 11 July | `f8cb5b0` | Phone transcription reported working with one microphone |
| 12 July | `36f3750` | Interim phone captions added; lower perceived delay reported |
| 15 July | `e728216` | Multi-line sentence display test |
| 22 July | `6653ff0` | XIAO microphone/display integration and serial bridge files |
| 22 July onward | `9e39a11` and development conversations | Bidirectional BLE integration, packet sizing, notification completion, and OLED debugging |
| Late July | Report and conference conversations | Technical report, Overleaf version, diagrams, and ten-minute presentation prepared |
| 28 July | Development conversation | View-through-glasses caption simulation requested and adjusted |
| 3 August | Development conversation | User questionnaire requested |
| 10 August | Development conversation | Phone drops compared with a stable laptop connection |
| 13 August | Development conversation | Microphone gain and sensitivity discussion |
| 17 August | Development conversation | BLE connection troubleshooting |
| 24 August | `06b4a3d` and development conversation | Audio/display changes, phone disconnects, gain concerns, dependency failures, and electrical observations |
| 10 September | Survey printout | Five-point Qualtrics questionnaire available |
| 15 September | `7721fba` and development conversation | Microphone/BLE changes committed; receiver delay investigated |
| 16 September | Development conversation | Bare-bones transcription milestone; laptop service-discovery disconnect |
| 17 September | `77e9975` | BLE and Deepgram decoupled; phone transcription reported responsive |
| 17 September | `5808fac` | Embedded firmware refactored into modules |
| 17 September | `51092cb`, `3c0374d` | BLE transport module committed; both client paths reported working |
| 17 September | `7b924df`, `7f30bcf` | Full pipeline reported working; display module committed |
| 17 September | `3264c26` | Microphone data and supply pin values reassigned |
| 18 September | `1aae8a2` and development conversation | Lens-viewed orientation and caption position changes |
| 23 to 25 September | Risk-assessment document | Assessment and dated researcher/supervisor entries |
| 5 October | Recent project conversations | Ethics-change response, removal of interviews, and two-speaker WER/latency planning |

## 19. Source register and continuation notes

### 19.1 Project documents reviewed

The following files are stored in the parent `P4P` directory:

- `Project Scope, Objectives & Literature Review.pdf`, dated 2 April 2026.
- `Mid_Year_Technical_Report_Tavish.pdf`.
- `transcription_glasses_survey.pdf`, with a 10 September 2026 printout date.
- `Risk_Assessment_Final.pdf`, with September assessment entries.
- `ParticipationForm.pdf` and `ConsentForm.pdf`.
- `Mid-Year_Technical_Report_Overleaf.zip`, inspected to identify its report and figure contents.

Previous-year reports were used as context in the earlier development and testing discussions. Their performance results are not treated as measurements of this year's prototype.

### 19.2 Source files reviewed

The most useful implementation records are:

```text
ble_receiver.py
ble_testing.py
one_mic_esp_data_export.cpp
testing/display_basic
testing/display_sentence
testing/mic-testing
TranscriberGlasses/TranscriberGlasses/BluetoothManager.swift
TranscriberGlasses/TranscriberGlasses/ContentView.swift
xiao_audio_display/README.md
xiao_audio_display/firmware/mic_test/mic_test.ino
xiao_audio_display/firmware/oled_mic_streamer/oled_mic_streamer.ino
xiao_audio_display/firmware/oled_mic_streamer/mic.cpp
xiao_audio_display/firmware/oled_mic_streamer/mic.h
xiao_audio_display/firmware/oled_mic_streamer/transport_ble.cpp
xiao_audio_display/firmware/oled_mic_streamer/transport.h
xiao_audio_display/firmware/oled_mic_streamer/display.cpp
xiao_audio_display/firmware/oled_mic_streamer/display.h
xiao_audio_display/python/serial_bridge.py
xiao_audio_display/python/protocol.py
```

Historical Git revisions were checked for the Whisper experiments, initial app milestones, serial pipeline, firmware refactor, microphone pin change, and final display revision.

### 19.3 Development conversations reviewed

The accessible local conversations covered mid-year report preparation, July BLE/OLED integration, conference planning, the display simulation, questionnaire preparation, August troubleshooting, September latency diagnosis, the bare-bones receiver, the working audio baseline, modular display integration, and lens orientation.

The recent app conversations reviewed were **Measure Latency WER**, **Supervisor Email Rewrite**, and **Improve wording**, all dated 5 October 2026. They supplied the latest testing direction and ethics changes.

### 19.4 Rules for extending this history

Future updates should record the date, hardware wiring, flashed firmware revision, phone build or laptop client, observed behaviour, and saved results. Keep user observations separate from numerical measurements, and keep proposed experiments separate from completed tests.

If a change replaces an earlier architecture, preserve the earlier stage here and update the current snapshot. The project has already used several capture, transport, buffering, and display arrangements; retaining those transitions makes later failures and design decisions easier to understand.
