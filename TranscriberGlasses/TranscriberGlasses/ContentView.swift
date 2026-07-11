//
//  ContentView.swift
//  TranscriberGlasses
//
//  Created by Tavish Puri on 08/07/2026.
//

import SwiftUI

private let deepgramAPIKey = "57490a6b4c360d6736e21b34c716649943e5e0a2"

struct ContentView: View {
    @StateObject private var bluetoothManager = BluetoothManager(apiKey: deepgramAPIKey)

    private let pageBackground = Color(red: 0.98, green: 0.97, blue: 0.93)
    private let panelBackground = Color(red: 1.0, green: 0.995, blue: 0.975)
    private let primaryText = Color(red: 0.08, green: 0.12, blue: 0.18)
    private let secondaryText = Color(red: 0.38, green: 0.44, blue: 0.52)
    private let accentBlue = Color(red: 0.12, green: 0.36, blue: 0.82)
    private let borderColor = Color(red: 0.77, green: 0.82, blue: 0.88)

    var body: some View {
        ZStack {
            background

            VStack(alignment: .leading, spacing: 20) {
                header
                statusPanel
                transcriptPanel
            }
            .padding(20)
        }
        .onAppear {
            bluetoothManager.start()
        }
    }

    private var background: some View {
        LinearGradient(
            colors: [pageBackground, Color(red: 0.93, green: 0.96, blue: 1.0)],
            startPoint: .top,
            endPoint: .bottom
        )
        .ignoresSafeArea()
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Transcriber Glasses")
                .font(.largeTitle.bold())
                .foregroundStyle(primaryText)

            Text("Live speech from ESP32-Audio")
                .font(.subheadline)
                .foregroundStyle(secondaryText)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var statusPanel: some View {
        HStack(spacing: 12) {
            Image(systemName: statusIconName)
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(statusColor)
                .frame(width: 36, height: 36)
                .background(statusColor.opacity(0.14), in: Circle())

            VStack(alignment: .leading, spacing: 4) {
                Text("Bluetooth")
                    .font(.caption)
                    .foregroundStyle(secondaryText)
                    .textCase(.uppercase)

                Text(bluetoothManager.connectionStatusText)
                    .font(.headline)
                    .foregroundStyle(primaryText)
            }

            Spacer()
        }
        .padding(16)
        .background(panelBackground, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .stroke(borderColor, lineWidth: 1)
        )
        .shadow(color: accentBlue.opacity(0.08), radius: 14, x: 0, y: 8)
    }

    private var transcriptPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("Transcript", systemImage: "text.quote")
                    .font(.headline)
                    .foregroundStyle(accentBlue)

                Spacer()

                Text(transcriptStateText)
                    .font(.caption)
                    .foregroundStyle(secondaryText)
            }

            ScrollView {
                Text(transcriptText)
                    .font(.body)
                    .foregroundStyle(bluetoothManager.transcript.isEmpty ? secondaryText : primaryText)
                    .lineSpacing(4)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 4)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(panelBackground, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .stroke(borderColor, lineWidth: 1)
        )
        .shadow(color: accentBlue.opacity(0.08), radius: 14, x: 0, y: 8)
    }

    private var transcriptText: String {
        bluetoothManager.transcript.isEmpty ? "Waiting for final transcript..." : bluetoothManager.transcript
    }

    private var transcriptStateText: String {
        bluetoothManager.transcript.isEmpty ? "Listening" : "Final text"
    }

    private var statusIconName: String {
        switch bluetoothManager.connectionState {
        case .scanning:
            return "dot.radiowaves.left.and.right"
        case .connected:
            return "checkmark.circle.fill"
        case .disconnected:
            return "xmark.circle.fill"
        }
    }

    private var statusColor: Color {
        switch bluetoothManager.connectionState {
        case .scanning:
            return accentBlue
        case .connected:
            return Color(red: 0.0, green: 0.48, blue: 0.34)
        case .disconnected:
            return Color(red: 0.78, green: 0.20, blue: 0.22)
        }
    }
}

#Preview {
    ContentView()
}
