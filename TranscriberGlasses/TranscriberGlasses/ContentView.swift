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

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(bluetoothManager.connectionStatusText)
                .font(.headline)

            ScrollView {
                Text(bluetoothManager.transcript.isEmpty ? "Waiting for transcript..." : bluetoothManager.transcript)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
        .onAppear {
            bluetoothManager.start()
        }
    }
}

#Preview {
    ContentView()
}
