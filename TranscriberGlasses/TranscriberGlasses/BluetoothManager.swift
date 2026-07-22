//
//  BluetoothManager.swift
//  TranscriberGlasses
//
//  Created by Tavish Puri on 08/07/2026.
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

    private let apiKey: String
    private let peripheralName = "ESP32-Audio"
    private let audioCharacteristicUUID = CBUUID(string: "12345678-1234-1234-1234-123456789abc")
    private let textCharacteristicUUID = CBUUID(string: "87654321-4321-4321-4321-cba987654321")
    private let deepgramURL = URL(string: "wss://api.deepgram.com/v1/listen?model=nova-3&language=en&encoding=linear16&sample_rate=16000&channels=1&interim_results=true&utterance_end_ms=1000")!

    private var centralManager: CBCentralManager?
    private var audioPeripheral: CBPeripheral?
    private var audioCharacteristic: CBCharacteristic?
    private var textCharacteristic: CBCharacteristic?
    private var webSocketTask: URLSessionWebSocketTask?
    private var reconnectWorkItem: DispatchWorkItem?
    private var oledUpdateWorkItem: DispatchWorkItem?
    private var lastOledText = ""
    private var lastOledWriteTime = Date.distantPast
    private let oledUpdateInterval: TimeInterval = 0.3

    // CoreBluetooth delivers many small audio notifications every second.
    // Keep that callback short, then send Deepgram a 100 ms PCM chunk.
    private let audioQueue = DispatchQueue(label: "com.transcriberglasses.audio")
    private var bufferedAudio = Data()
    private var audioFlushTimer: DispatchSourceTimer?
    private let audioFlushInterval: DispatchTimeInterval = .milliseconds(100)
    private let maximumBufferedAudioBytes = 16_000

    init(apiKey: String) {
        self.apiKey = apiKey
        super.init()
    }

    func start() {
        if centralManager == nil {
            centralManager = CBCentralManager(delegate: self, queue: .main)
        } else if centralManager?.state == .poweredOn {
            scanForPeripheral()
        }
    }

    private func scanForPeripheral() {
        reconnectWorkItem?.cancel()
        connectedPeripheralName = nil
        connectionState = .scanning
        centralManager?.scanForPeripherals(withServices: [audioCharacteristicUUID])
    }

    private func connectWebSocket() {
        guard webSocketTask == nil else { return }

        var request = URLRequest(url: deepgramURL)
        request.addValue("Token \(apiKey)", forHTTPHeaderField: "Authorization")

        let task = URLSession.shared.webSocketTask(with: request)
        webSocketTask = task
        task.resume()
        startAudioForwarding()
        receiveDeepgramMessage(from: task)
    }

    private func startAudioForwarding() {
        audioQueue.async { [weak self] in
            guard let self, self.audioFlushTimer == nil else { return }

            let timer = DispatchSource.makeTimerSource(queue: self.audioQueue)
            timer.schedule(deadline: .now() + self.audioFlushInterval, repeating: self.audioFlushInterval)
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
            self.bufferedAudio.removeAll(keepingCapacity: false)
        }
    }

    private func queueAudioForDeepgram(_ data: Data) {
        audioQueue.async { [weak self] in
            guard let self else { return }

            self.bufferedAudio.append(data)

            // Do not let a slow network connection grow memory forever.
            if self.bufferedAudio.count > self.maximumBufferedAudioBytes {
                print("Deepgram audio buffer overflow, dropping buffered audio")
                self.bufferedAudio.removeAll(keepingCapacity: true)
            }
        }
    }

    // Called only from audioQueue.
    private func flushBufferedAudio() {
        guard !bufferedAudio.isEmpty, let webSocketTask else { return }

        let audioChunk = bufferedAudio
        bufferedAudio.removeAll(keepingCapacity: true)

        webSocketTask.send(.data(audioChunk)) { [weak self, weak webSocketTask] error in
            guard let self, self.webSocketTask === webSocketTask else { return }
            if let error, (error as NSError).code != NSURLErrorCancelled {
                print("Deepgram send failed: \(error.localizedDescription)")
            }
        }
    }

    private func receiveDeepgramMessage(from task: URLSessionWebSocketTask) {
        task.receive { [weak self, weak task] result in
            guard let self else { return }
            guard let task, self.webSocketTask === task else { return }

            switch result {
            case .success(let message):
                self.handleDeepgramMessage(message)
                self.receiveDeepgramMessage(from: task)
            case .failure(let error):
                if (error as NSError).code != NSURLErrorCancelled {
                    print("Deepgram receive failed: \(error.localizedDescription)")
                }
                self.stopAudioForwarding()
                if self.webSocketTask === task {
                    self.webSocketTask = nil
                }
            }
        }
    }

    private func handleDeepgramMessage(_ message: URLSessionWebSocketTask.Message) {
        let data: Data?

        switch message {
        case .string(let text):
            data = text.data(using: .utf8)
        case .data(let messageData):
            data = messageData
        @unknown default:
            data = nil
        }

        guard let data,
              let response = try? JSONDecoder().decode(DeepgramResponse.self, from: data),
              let text = response.channel.alternatives.first?.transcript else {
            return
        }

        DispatchQueue.main.async {
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
                self.queueTranscriptForOled(text, immediately: true)
            } else {
                self.interimTranscript = text
                self.queueTranscriptForOled(text, immediately: false)
            }
        }
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            scanForPeripheral()
        } else {
            connectionState = .disconnected
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertisedName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard peripheral.name == peripheralName || advertisedName == peripheralName else { return }

        audioPeripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedPeripheralName = peripheral.name ?? peripheralName
        connectionState = .connected
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("BLE disconnected: \(error?.localizedDescription ?? "no error reported")")
        connectedPeripheralName = nil
        connectionState = .disconnected
        audioCharacteristic = nil
        textCharacteristic = nil
        oledUpdateWorkItem?.cancel()
        oledUpdateWorkItem = nil
        lastOledText = ""
        stopAudioForwarding()
        webSocketTask?.cancel(with: .normalClosure, reason: nil)
        webSocketTask = nil
        scheduleReconnect()
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        print("BLE connection failed: \(error?.localizedDescription ?? "no error reported")")
        connectedPeripheralName = nil
        connectionState = .disconnected
        scheduleReconnect()
    }

    private func scheduleReconnect() {
        reconnectWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            guard let self, self.centralManager?.state == .poweredOn else { return }
            self.scanForPeripheral()
        }
        reconnectWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: workItem)
    }
}

extension BluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }

        for service in services {
            peripheral.discoverCharacteristics([audioCharacteristicUUID, textCharacteristicUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil else {
            print("Characteristic discovery failed: \(error!.localizedDescription)")
            return
        }
        guard let characteristics = service.characteristics else { return }

        for characteristic in characteristics {
            switch characteristic.uuid {
            case audioCharacteristicUUID:
                audioCharacteristic = characteristic
            case textCharacteristicUUID:
                textCharacteristic = characteristic
                let testMessage = Data("iPhone connected".utf8)
                peripheral.writeValue(testMessage, for: characteristic, type: .withoutResponse)
                print("OLED text characteristic ready; sent connection test")
            default:
                break
            }
        }

        if let audioCharacteristic {
            peripheral.setNotifyValue(true, for: audioCharacteristic)
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard characteristic.uuid == audioCharacteristicUUID else { return }

        guard error == nil, characteristic.isNotifying else {
            print("Could not enable audio notifications: \(error?.localizedDescription ?? "unknown error")")
            return
        }

        // Only start the paid network stream once BLE audio is flowing.
        connectWebSocket()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            print("Audio notification failed: \(error.localizedDescription)")
            return
        }
        guard characteristic.uuid == audioCharacteristicUUID,
              let data = characteristic.value else {
            return
        }

        // Do not send to Deepgram directly from this CoreBluetooth callback.
        queueAudioForDeepgram(data)
    }

    private func sendTranscriptToOled(_ text: String) {
        guard let audioPeripheral, let textCharacteristic else { return }

        let payload = Data(text.utf8.prefix(192))
        audioPeripheral.writeValue(payload, for: textCharacteristic, type: .withoutResponse)
        print("Sent OLED text: \(text)")
    }

    private func queueTranscriptForOled(_ text: String, immediately: Bool) {
        guard !text.isEmpty, text != lastOledText else { return }

        oledUpdateWorkItem?.cancel()
        oledUpdateWorkItem = nil

        let elapsed = Date().timeIntervalSince(lastOledWriteTime)
        let delay = immediately ? 0 : max(0, oledUpdateInterval - elapsed)

        let workItem = DispatchWorkItem { [weak self] in
            guard let self, text != self.lastOledText else { return }
            self.lastOledText = text
            self.lastOledWriteTime = Date()
            self.sendTranscriptToOled(text)
            self.oledUpdateWorkItem = nil
        }

        if delay == 0 {
            workItem.perform()
        } else {
            oledUpdateWorkItem = workItem
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: workItem)
        }
    }
}

private struct DeepgramResponse: Decodable {
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
