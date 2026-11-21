import SwiftUI

/// The companion's only screen. R1 spike version: connection state, probe results, bind switch, log.
struct StatusView: View {
    @ObservedObject var model: CompanionModel
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack(spacing: 12) {
                        Circle()
                            .fill(model.isConnected ? Color.green : Color.orange)
                            .frame(width: 12, height: 12)
                        Text(model.stateLine)
                            .font(.headline)
                    }
                    if let pc = model.pcDescription {
                        LabeledContent("PC", value: pc)
                    }
                    Text("Keep this app open while the PC is connected. iOS stops it in the background.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                Section("Link test") {
                    LabeledContent("Phone → PC", value: format(model.lastDownMbps))
                    LabeledContent("PC → phone", value: format(model.lastUpMbps))
                    LabeledContent("Pings answered", value: "\(model.pingCount)")
                }

                Section {
                    Picker("Listen on", selection: $model.bindMode) {
                        Text("Loopback").tag(SocketServer.BindMode.loopback)
                        Text("All interfaces").tag(SocketServer.BindMode.any)
                    }
                    .pickerStyle(.segmented)
                } header: {
                    Text("Spike setting")
                } footer: {
                    Text("Loopback should be enough for USB. Switch only if the PC can't connect, and note whether iOS asks for Local Network access.")
                }

                Section("App") {
                    LabeledContent("Version", value: "\(DeviceInfo.appVersion) (\(DeviceInfo.buildNumber))")
                    LabeledContent("Protocol", value: WireProtocol.versionString)
                    LabeledContent("Signature expires", value: expiryText)
                }

                Section("Log") {
                    ForEach(Array(model.logLines.suffix(40).reversed().enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(.caption.monospaced())
                            .textSelection(.enabled)
                    }
                }
            }
            .navigationTitle(DeviceInfo.displayName)
        }
        .onChange(of: scenePhase, initial: true) { _, phase in
            switch phase {
            case .active: model.start()
            case .background: model.stop()
            default: break
            }
        }
    }

    private var expiryText: String {
        guard let date = model.signingExpiry else { return "Unknown" }
        return date.formatted(.relative(presentation: .named))
    }

    private func format(_ mbps: Double?) -> String {
        guard let mbps else { return "–" }
        return String(format: "%.0f Mbit/s", mbps)
    }
}
