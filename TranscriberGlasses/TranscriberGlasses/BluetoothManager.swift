//
//  BluetoothManager.swift
//  TranscriberGlasses
//

import Combine
import CoreBluetooth
import Foundation

final class BluetoothManager: NSObject, ObservableObject {

    enum ConnectionState: String {
        case scanning = "Scanning"
        case connected = "Connected"
        case disconnected = "Disconnected"
    }

    // MARK: - Published state

    @Published private(set) var connectionState: ConnectionState = .disconnected
    @Published private(set) var connectedPeripheralName: String?
    @Published private(set) var transcript = ""
    @Published private(set) var interimTranscript = ""

    var displayedTranscript: String {
        switch (transcript.isEmpty, interimTranscript.isEmpty) {
        case (true, true):
            return ""

        case (true, false):
            return interimTranscript

        case (false, true):
            return transcript

        case (false, false):
            return "\(transcript) \(interimTranscript)"
        }
    }

    var connectionStatusText: String {
        switch connectionState {
        case .scanning:
            return "Scanning for ESP32-Audio"

        case .connected:
            return "Connected to \(connectedPeripheralName ?? peripheralName)"

        case .disconnected:
            return "Disconnected"
        }
    }

    // MARK: - Configuration

    private let apiKey: String

    private let peripheralName = "ESP32-Audio"

    private let audioCharacteristicUUID =
        CBUUID(string: "12345678-1234-1234-1234-123456789abc")

    private let textCharacteristicUUID =
        CBUUID(string: "87654321-4321-4321-4321-cba987654321")

    private let deepgramURL = URL(
        string:
            "wss://api.deepgram.com/v1/listen" +
            "?model=nova-3" +
            "&language=en" +
            "&encoding=linear16" +
            "&sample_rate=16000" +
            "&channels=1" +
            "&interim_results=true" +
            "&utterance_end_ms=1000"
    )!

    // MARK: - CoreBluetooth

    private var centralManager: CBCentralManager?
    private var audioPeripheral: CBPeripheral?

    private var audioCharacteristic: CBCharacteristic?
    private var textCharacteristic: CBCharacteristic?

    private var reconnectWorkItem: DispatchWorkItem?

    // MARK: - Deepgram

    // All Deepgram connection state lives on this serial queue.
    // This prevents races between send failures, receive failures,
    // reconnects, and BLE callbacks.
    private let deepgramQueue =
        DispatchQueue(label: "com.transcriberglasses.deepgram")

    private var webSocketTask: URLSessionWebSocketTask?
    private var deepgramReconnectWorkItem: DispatchWorkItem?
    private var keepAliveTimer: DispatchSourceTimer?

    // True while BLE audio notifications are active.
    // Deepgram should exist only while this is true.
    private var shouldRunDeepgram = false

    private let deepgramReconnectDelay: TimeInterval = 1.0
    private let keepAliveInterval: TimeInterval = 4.0

    // MARK: - Audio buffering

    // CoreBluetooth sends lots of tiny packets.
    //
    // BLE callback:
    //     packet -> audioQueue -> return immediately
    //
    // Every 100 ms:
    //     buffered PCM -> Deepgram queue -> websocket
    //
    // Therefore CoreBluetooth never waits on the network.
    private let audioQueue =
        DispatchQueue(label: "com.transcriberglasses.audio")

    private var bufferedAudio = Data()
    private var audioFlushTimer: DispatchSourceTimer?

    private let audioFlushInterval: DispatchTimeInterval =
        .milliseconds(100)

    // 32 kB = ~1 second of 16 kHz PCM16 mono.
    private let maximumBufferedAudioBytes = 32_000

    // MARK: - Init

    init(apiKey: String) {
        self.apiKey = apiKey
        super.init()
    }

    // MARK: - Start

    func start() {
        if centralManager == nil {
            centralManager =
                CBCentralManager(
                    delegate: self,
                    queue: .main
                )

        } else if centralManager?.state == .poweredOn {
            scanForPeripheral()
        }
    }

    // MARK: - BLE scanning

    private func scanForPeripheral() {
        reconnectWorkItem?.cancel()

        connectedPeripheralName = nil
        connectionState = .scanning

        // Match the device name in didDiscover. A service-filtered scan only
        // returns peripherals whose advertising data includes that service.
        centralManager?.scanForPeripherals(
            withServices: nil
        )

        print("BLE scanning for \(peripheralName)")
    }

    private func scheduleBLEReconnect() {
        reconnectWorkItem?.cancel()

        print("BLE reconnect scan scheduled")

        let workItem = DispatchWorkItem { [weak self] in
            guard let self else { return }

            guard self.centralManager?.state == .poweredOn else {
                print("BLE reconnect scan skipped: Bluetooth unavailable")
                return
            }

            self.scanForPeripheral()
        }

        reconnectWorkItem = workItem

        DispatchQueue.main.asyncAfter(
            deadline: .now() + 1,
            execute: workItem
        )
    }

    // MARK: - Audio buffering

    private func startAudioForwarding() {
        audioQueue.async { [weak self] in
            guard let self else { return }

            guard self.audioFlushTimer == nil else {
                return
            }

            let timer =
                DispatchSource.makeTimerSource(
                    queue: self.audioQueue
                )

            timer.schedule(
                deadline: .now() + self.audioFlushInterval,
                repeating: self.audioFlushInterval
            )

            timer.setEventHandler { [weak self] in
                self?.flushBufferedAudio()
            }

            self.audioFlushTimer = timer

            timer.resume()
        }
    }

    private func stopAudioForwarding() {
        audioQueue.async { [weak self] in
            guard let self else { return }

            self.audioFlushTimer?.setEventHandler {}
            self.audioFlushTimer?.cancel()

            self.audioFlushTimer = nil

            // Do not replay old speech if BLE reconnects later.
            self.bufferedAudio.removeAll(
                keepingCapacity: false
            )
        }
    }

    private func queueAudioForDeepgram(_ data: Data) {
        audioQueue.async { [weak self] in
            guard let self else { return }

            self.bufferedAudio.append(data)

            // Live transcription cares about NEW audio.
            //
            // If something somehow stalls this queue, retain the newest
            // audio instead of allowing memory to grow forever or clearing
            // the whole buffer.
            if self.bufferedAudio.count >
                self.maximumBufferedAudioBytes {

                self.bufferedAudio =
                    Data(
                        self.bufferedAudio.suffix(
                            self.maximumBufferedAudioBytes
                        )
                    )

                print(
                    "Audio buffer overflow - dropped oldest audio"
                )
            }
        }
    }

    // Called only from audioQueue.
    private func flushBufferedAudio() {
        guard !bufferedAudio.isEmpty else {
            return
        }

        let audioChunk = bufferedAudio

        bufferedAudio.removeAll(
            keepingCapacity: true
        )

        // Network work is NOT performed on audioQueue.
        // Hand it to the dedicated Deepgram queue.
        sendAudioToDeepgram(audioChunk)
    }

    // MARK: - Deepgram lifecycle

    private func startDeepgram() {
        deepgramQueue.async { [weak self] in
            guard let self else { return }

            self.shouldRunDeepgram = true

            self.connectDeepgramIfNeeded()
        }
    }

    private func stopDeepgram() {
        deepgramQueue.async { [weak self] in
            guard let self else { return }

            self.shouldRunDeepgram = false

            self.deepgramReconnectWorkItem?.cancel()
            self.deepgramReconnectWorkItem = nil

            self.stopKeepAlive()

            let task = self.webSocketTask

            self.webSocketTask = nil

            task?.cancel(
                with: .normalClosure,
                reason: nil
            )
        }
    }

    // Must only be called from deepgramQueue.
    private func connectDeepgramIfNeeded() {
        guard shouldRunDeepgram else {
            return
        }

        guard webSocketTask == nil else {
            return
        }

        deepgramReconnectWorkItem?.cancel()
        deepgramReconnectWorkItem = nil

        print("Connecting to Deepgram...")

        var request =
            URLRequest(url: deepgramURL)

        request.addValue(
            "Token \(apiKey)",
            forHTTPHeaderField: "Authorization"
        )

        let task =
            URLSession.shared.webSocketTask(
                with: request
            )

        webSocketTask = task

        task.resume()

        print("Deepgram websocket started")

        startKeepAlive()

        receiveDeepgramMessage(from: task)
    }

    // MARK: - Deepgram reconnection

    // Must only be called from deepgramQueue.
    private func handleDeepgramFailure(
        task: URLSessionWebSocketTask,
        reason: String
    ) {
        // Ignore errors from an old socket that has already been replaced.
        guard webSocketTask === task else {
            return
        }

        print("Deepgram connection lost: \(reason)")

        stopKeepAlive()

        webSocketTask = nil

        task.cancel(
            with: .goingAway,
            reason: nil
        )

        // Throw away any audio accumulated around the failure.
        //
        // Sending old audio after reconnecting would add latency and make
        // live captions confusing.
        audioQueue.async { [weak self] in
            self?.bufferedAudio.removeAll(
                keepingCapacity: true
            )
        }

        scheduleDeepgramReconnect()
    }

    // Must only be called from deepgramQueue.
    private func scheduleDeepgramReconnect() {
        guard shouldRunDeepgram else {
            return
        }

        // Prevent several send/receive errors creating several reconnects.
        guard deepgramReconnectWorkItem == nil else {
            return
        }

        print("Deepgram reconnect scheduled")

        let workItem = DispatchWorkItem { [weak self] in
            guard let self else { return }

            self.deepgramReconnectWorkItem = nil

            guard self.shouldRunDeepgram else {
                return
            }

            print("Reconnecting to Deepgram...")

            self.connectDeepgramIfNeeded()
        }

        deepgramReconnectWorkItem = workItem

        deepgramQueue.asyncAfter(
            deadline: .now() + deepgramReconnectDelay,
            execute: workItem
        )
    }

    // MARK: - Deepgram sending

    private func sendAudioToDeepgram(
        _ audioChunk: Data
    ) {
        deepgramQueue.async { [weak self] in
            guard let self else { return }

            // If Deepgram is temporarily reconnecting, simply discard this
            // live chunk. Fresh speech will start flowing after reconnection.
            guard let task = self.webSocketTask else {
                return
            }

            task.send(
                .data(audioChunk)
            ) { [weak self, weak task] error in

                guard let self else {
                    return
                }

                guard let task else {
                    return
                }

                guard let error else {
                    return
                }

                if (error as NSError).code ==
                    NSURLErrorCancelled {
                    return
                }

                self.deepgramQueue.async {
                    self.handleDeepgramFailure(
                        task: task,
                        reason:
                            "send failed: \(error.localizedDescription)"
                    )
                }
            }
        }
    }

    // MARK: - Deepgram receiving

    private func receiveDeepgramMessage(
        from task: URLSessionWebSocketTask
    ) {
        task.receive { [weak self, weak task] result in
            guard let self else {
                return
            }

            guard let task else {
                return
            }

            self.deepgramQueue.async {

                // Ignore callbacks belonging to an old socket.
                guard self.webSocketTask === task else {
                    return
                }

                switch result {

                case .success(let message):

                    // Parsing/UI work does not need to block connection state.
                    self.handleDeepgramMessage(message)

                    // Continue listening.
                    self.receiveDeepgramMessage(
                        from: task
                    )

                case .failure(let error):

                    if (error as NSError).code ==
                        NSURLErrorCancelled {
                        return
                    }

                    self.handleDeepgramFailure(
                        task: task,
                        reason:
                            "receive failed: \(error.localizedDescription)"
                    )
                }
            }
        }
    }

    // MARK: - Deepgram KeepAlive

    // Must only be called from deepgramQueue.
    private func startKeepAlive() {
        stopKeepAlive()

        let timer =
            DispatchSource.makeTimerSource(
                queue: deepgramQueue
            )

        timer.schedule(
            deadline: .now() + keepAliveInterval,
            repeating: keepAliveInterval
        )

        timer.setEventHandler { [weak self] in
            guard let self else {
                return
            }

            guard let task =
                self.webSocketTask else {
                return
            }

            let keepAlive =
                #"{"type":"KeepAlive"}"#

            task.send(
                .string(keepAlive)
            ) { [weak self, weak task] error in

                guard let self else {
                    return
                }

                guard let task else {
                    return
                }

                guard let error else {
                    return
                }

                if (error as NSError).code ==
                    NSURLErrorCancelled {
                    return
                }

                self.deepgramQueue.async {
                    self.handleDeepgramFailure(
                        task: task,
                        reason:
                            "keepalive failed: \(error.localizedDescription)"
                    )
                }
            }
        }

        keepAliveTimer = timer

        timer.resume()
    }

    // Must only be called from deepgramQueue.
    private func stopKeepAlive() {
        keepAliveTimer?.setEventHandler {}
        keepAliveTimer?.cancel()

        keepAliveTimer = nil
    }

    // MARK: - Deepgram response parsing

    private func handleDeepgramMessage(
        _ message: URLSessionWebSocketTask.Message
    ) {
        let data: Data?

        switch message {

        case .string(let text):
            data = text.data(using: .utf8)

        case .data(let messageData):
            data = messageData

        @unknown default:
            data = nil
        }

        guard
            let data,
            let response =
                try? JSONDecoder().decode(
                    DeepgramResponse.self,
                    from: data
                ),
            let text =
                response.channel
                    .alternatives
                    .first?
                    .transcript
        else {
            // Deepgram also sends metadata / keepalive responses.
            // Those are intentionally ignored.
            return
        }

        DispatchQueue.main.async { [weak self] in
            guard let self else {
                return
            }

            if response.isFinal {

                guard !text.isEmpty else {
                    self.interimTranscript = ""
                    return
                }

                if self.transcript.isEmpty {
                    self.transcript = text
                } else {
                    self.transcript += " \(text)"
                }

                self.interimTranscript = ""

                self.sendFinalTranscriptToESP(text)

            } else {

                self.interimTranscript = text

            }
        }
    }

    // MARK: - Transcript return

    private func sendFinalTranscriptToESP(_ text: String) {
        guard
            let audioPeripheral,
            audioPeripheral.state == .connected,
            let textCharacteristic,
            textCharacteristic.properties.contains(.writeWithoutResponse)
        else {
            print("BLE final text skipped: write-without-response characteristic unavailable")
            return
        }

        // Keep each write within one negotiated ATT payload, capped at 180 bytes.
        let maxLength = min(
            180,
            audioPeripheral.maximumWriteValueLength(for: .withoutResponse)
        )

        guard maxLength > 0 else {
            print("BLE final text skipped: write length unavailable")
            return
        }

        var payload = Data(text.utf8.prefix(maxLength))

        // A byte limit can cut through a UTF-8 character.
        while !payload.isEmpty && String(data: payload, encoding: .utf8) == nil {
            payload.removeLast()
        }

        guard !payload.isEmpty else {
            return
        }

        audioPeripheral.writeValue(
            payload,
            for: textCharacteristic,
            type: .withoutResponse
        )

        print("BLE final text write queued without response: \(payload.count) bytes")
        if payload.count < text.utf8.count {
            print("BLE final text truncated to fit one packet")
        }
    }
}


// MARK: - CBCentralManagerDelegate

extension BluetoothManager:
    CBCentralManagerDelegate {

    func centralManagerDidUpdateState(
        _ central: CBCentralManager
    ) {
        if central.state == .poweredOn {

            scanForPeripheral()

        } else {

            connectionState = .disconnected

            stopAudioForwarding()
            stopDeepgram()
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertisedName =
            advertisementData[
                CBAdvertisementDataLocalNameKey
            ] as? String

        guard
            peripheral.name == peripheralName ||
            advertisedName == peripheralName
        else {
            return
        }

        audioPeripheral = peripheral
        peripheral.delegate = self

        print(
            "BLE found \(peripheralName): " +
            "\(peripheral.identifier), RSSI \(RSSI)"
        )

        central.stopScan()

        print("BLE connecting to \(peripheral.identifier)")
        central.connect(peripheral)
    }

    func centralManager(
        _ central: CBCentralManager,
        didConnect peripheral: CBPeripheral
    ) {
        print("BLE connected to \(peripheral.identifier)")

        connectedPeripheralName =
            peripheral.name ?? peripheralName

        connectionState = .connected

        peripheral.discoverServices(nil)
    }

    func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        if let error {
            let detail = error as NSError
            print(
                "BLE disconnected \(peripheral.identifier): " +
                "\(detail.domain) code \(detail.code), " +
                detail.localizedDescription
            )
        } else {
            print(
                "BLE disconnected \(peripheral.identifier): " +
                "no error reported"
            )
        }

        connectedPeripheralName = nil
        connectionState = .disconnected

        audioCharacteristic = nil
        textCharacteristic = nil

        // BLE and Deepgram are independent.
        //
        // Stop Deepgram cleanly because there is no longer an audio source.
        stopAudioForwarding()
        stopDeepgram()

        scheduleBLEReconnect()
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        print(
            "BLE connection failed \(peripheral.identifier): " +
            "\(error?.localizedDescription ?? "no error reported")"
        )

        connectedPeripheralName = nil
        connectionState = .disconnected

        stopAudioForwarding()
        stopDeepgram()

        scheduleBLEReconnect()
    }
}


// MARK: - CBPeripheralDelegate

extension BluetoothManager:
    CBPeripheralDelegate {

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverServices error: Error?
    ) {
        if let error {
            print(
                "Service discovery failed: " +
                error.localizedDescription
            )

            return
        }

        guard let services =
            peripheral.services else {
            return
        }

        print("BLE services discovered: \(services.count)")

        for service in services {

            peripheral.discoverCharacteristics(
                [
                    audioCharacteristicUUID,
                    textCharacteristicUUID
                ],
                for: service
            )
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            print(
                "Characteristic discovery failed: " +
                error.localizedDescription
            )

            return
        }

        guard let characteristics =
            service.characteristics else {
            return
        }

        for characteristic in characteristics {

            switch characteristic.uuid {

            case audioCharacteristicUUID:

                audioCharacteristic =
                    characteristic

                print("BLE audio characteristic ready")

            case textCharacteristicUUID:

                textCharacteristic =
                    characteristic

                print("BLE text characteristic ready")

            default:
                break
            }
        }

        if let audioCharacteristic {

            peripheral.setNotifyValue(
                true,
                for: audioCharacteristic
            )
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard
            characteristic.uuid ==
                audioCharacteristicUUID
        else {
            return
        }

        guard
            error == nil,
            characteristic.isNotifying
        else {

            print(
                "Could not enable audio notifications: " +
                "\(error?.localizedDescription ?? "unknown error")"
            )

            return
        }

        print(
            "BLE audio notifications enabled"
        )

        // Start the local audio batching independently from Deepgram.
        startAudioForwarding()

        // Only now start the paid/network transcription stream.
        startDeepgram()
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {

            print(
                "Audio notification failed: " +
                error.localizedDescription
            )

            return
        }

        guard
            characteristic.uuid ==
                audioCharacteristicUUID,
            let data =
                characteristic.value
        else {
            return
        }

        // IMPORTANT:
        //
        // No networking happens in this CoreBluetooth callback.
        //
        // BLE -> queue -> return.
        queueAudioForDeepgram(data)
    }
}


// MARK: - Deepgram JSON models

private struct DeepgramResponse:
    Decodable {

    let isFinal: Bool
    let channel: Channel

    enum CodingKeys: String, CodingKey {
        case isFinal = "is_final"
        case channel
    }

    struct Channel: Decodable {
        let alternatives: [Alternative]
    }

    struct Alternative: Decodable {
        let transcript: String
    }
}
