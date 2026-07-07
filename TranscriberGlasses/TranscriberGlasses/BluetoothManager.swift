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
    private let deepgramURL = URL(string: "wss://api.deepgram.com/v1/listen?model=nova-3&language=en&encoding=linear16&sample_rate=16000&channels=1&interim_results=true&utterance_end_ms=1000")!

    private var centralManager: CBCentralManager?
    private var audioPeripheral: CBPeripheral?
    private var audioCharacteristic: CBCharacteristic?
    private var webSocketTask: URLSessionWebSocketTask?

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
        connectedPeripheralName = nil
        connectionState = .scanning
        centralManager?.scanForPeripherals(withServices: nil)
    }

    private func connectWebSocket() {
        var request = URLRequest(url: deepgramURL)
        request.addValue("Token \(apiKey)", forHTTPHeaderField: "Authorization")

        let task = URLSession.shared.webSocketTask(with: request)
        webSocketTask = task
        task.resume()
        receiveDeepgramMessage()
    }

    private func sendAudioPacket(_ data: Data) {
        webSocketTask?.send(.data(data)) { error in
            if let error {
                print("Deepgram send failed: \(error.localizedDescription)")
            }
        }
    }

    private func receiveDeepgramMessage() {
        webSocketTask?.receive { [weak self] result in
            guard let self else { return }

            switch result {
            case .success(let message):
                self.handleDeepgramMessage(message)
                self.receiveDeepgramMessage()
            case .failure(let error):
                print("Deepgram receive failed: \(error.localizedDescription)")
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
              response.isFinal,
              let text = response.channel.alternatives.first?.transcript,
              !text.isEmpty else {
            return
        }

        DispatchQueue.main.async {
            if self.transcript.isEmpty {
                self.transcript = text
            } else {
                self.transcript += " \(text)"
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
        guard peripheral.name == peripheralName else { return }

        audioPeripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedPeripheralName = peripheral.name ?? peripheralName
        connectionState = .connected
        connectWebSocket()
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        connectedPeripheralName = nil
        connectionState = .disconnected
        webSocketTask?.cancel(with: .normalClosure, reason: nil)
        webSocketTask = nil
    }
}

extension BluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }

        for service in services {
            peripheral.discoverCharacteristics([audioCharacteristicUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }

        for characteristic in characteristics where characteristic.uuid == audioCharacteristicUUID {
            audioCharacteristic = characteristic
            peripheral.setNotifyValue(true, for: characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == audioCharacteristicUUID,
              let data = characteristic.value else {
            return
        }

        sendAudioPacket(data)
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
