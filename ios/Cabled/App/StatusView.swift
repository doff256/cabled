import AVFoundation
import SwiftUI

/// The companion's only screen (docs/ios-companion.md): what's happening, what to do next, when the
/// signature runs out, the version. Everything adjustable lives on Windows.
struct StatusView: View {
    @ObservedObject var model: CompanionModel
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openURL) private var openURL
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var showDetails = false
    @State private var appeared = false

    var body: some View {
        let state = ScreenState(model: model)
        VStack(alignment: .leading, spacing: 14) {
            header(state).entrance(appeared, step: 0)
            hero(state).entrance(appeared, step: 1)
            if let stream = state.stream {
                readout(stream).entrance(appeared, step: 2)
                    .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
            signatureCard.entrance(appeared, step: 3)
            Spacer(minLength: 0)
            bottomBar(state).entrance(appeared, step: 4)
        }
        .padding(.horizontal, 18)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(MG.surface100.ignoresSafeArea())
        .foregroundStyle(MG.ink)
        .animation(reduceMotion ? nil : MG.move, value: state.stream)
        .sheet(isPresented: $showDetails) { DetailsSheet(model: model) }
        .onAppear { appeared = true }
        .onChange(of: scenePhase, initial: true) { _, phase in
            switch phase {
            case .active: model.start()
            case .background: model.stop()
            default: break
            }
        }
    }

    // MARK: Parts

    private func header(_ state: ScreenState) -> some View {
        HStack(spacing: 8) {
            IrisView(open: 0.2, breathing: false).frame(width: 24, height: 24)
            Text(DeviceInfo.displayName).font(MG.display(20)).tracking(-0.4)
            Spacer()
            StatusPill(word: state.pill, color: state.pillColor, breathing: state.isLive)
                .animation(reduceMotion ? nil : MG.move, value: state.pill)
        }
    }

    private func hero(_ state: ScreenState) -> some View {
        VStack(spacing: 14) {
            IrisView(open: state.iris, breathing: state.isLive)
                .frame(width: 168, height: 168)
                .padding(.top, 6)
            Text(state.title)
                .font(MG.display(27))
                .tracking(-0.6)
                .multilineTextAlignment(.center)
                .contentTransition(.opacity)
            Text(state.detail)
                .font(MG.text(14.5))
                .foregroundStyle(MG.inkMuted)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 300)
                .contentTransition(.opacity)
        }
        .animation(reduceMotion ? nil : MG.move, value: state.title)
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 18)
        .padding(.top, 18)
        .padding(.bottom, 22)
        .mgWell()
        .accessibilityElement(children: .combine)
    }

    private func readout(_ stream: StreamInfo) -> some View {
        VStack(spacing: 0) {
            row("Output", value: "\(stream.width) × \(stream.height)")
            row("Frame rate", value: "\(stream.fps)", unit: "fps")
            row("Codec", value: stream.hevc ? "HEVC" : "H.264")
            row("Phone", value: ScreenState.heatWord(model.thermal),
                color: model.thermal.rawValue >= ProcessInfo.ThermalState.serious.rawValue ? MG.clay : MG.ink)
        }
    }

    private func row(_ label: String, value: String, unit: String? = nil, color: Color = MG.ink) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).font(MG.text(14)).foregroundStyle(MG.inkMuted)
            Spacer()
            Text(value).font(MG.mono(14.5)).foregroundStyle(color).monospacedDigit()
            if let unit { Text(unit).font(MG.mono(11.5, .regular)).foregroundStyle(MG.inkFaint) }
        }
        .padding(.vertical, 9)
        .padding(.horizontal, 2)
        .overlay(alignment: .bottom) { Rectangle().fill(MG.line).frame(height: 1) }
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder private var signatureCard: some View {
        if let days = signatureDays {
            let soon = days < 2
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("App signature").font(MG.text(14, .medium))
                    Spacer()
                    Text(days <= 0 ? "Expired" : days == 1 ? "1 day" : "\(days) days")
                        .font(MG.mono(14))
                        .foregroundStyle(soon ? MG.clay : MG.ink)
                }
                Text(soon ? "Open SideStore and refresh \(DeviceInfo.displayName), or it stops opening."
                          : "Refresh in SideStore before it runs out.")
                    .font(MG.text(13))
                    .foregroundStyle(MG.inkMuted)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .mgCard(edge: soon ? MG.clay.opacity(0.45) : MG.line)
            .accessibilityElement(children: .combine)
        }
    }

    private func bottomBar(_ state: ScreenState) -> some View {
        HStack(spacing: 8) {
            Text("\(DeviceInfo.appVersion) (\(DeviceInfo.buildNumber)) · protocol \(WireProtocol.versionString)")
                .font(MG.mono(11.5, .regular))
                .foregroundStyle(MG.inkFaint)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
            Spacer(minLength: 4)
            if state.cameraOff {
                Button("Open Settings") {
                    if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
                }
                .buttonStyle(LacquerButtonStyle())
            } else if let days = signatureDays, days < 2 {
                Button("Open SideStore") {
                    if let url = URL(string: "sidestore://") { openURL(url) }
                }
                .buttonStyle(LacquerButtonStyle())
            }
            Button("Details") { showDetails = true }
                .buttonStyle(GhostButtonStyle())
        }
        .padding(.leading, 16)
        .padding(.trailing, 6)
        .padding(.vertical, 6)
        .mgGlass(in: Capsule())
    }

    private var signatureDays: Int? {
        guard let expiry = model.signingExpiry else { return nil }
        return Int((expiry.timeIntervalSinceNow / 86_400).rounded(.up))
    }
}

/// What the screen shows for the model's current state.
private struct ScreenState {
    var pill = "STARTING"
    var pillColor = MG.inkFaint
    var iris = 0.12
    var title = "Starting"
    var detail = " "
    var stream: StreamInfo?
    var isLive = false
    var cameraOff = false

    init(model: CompanionModel) {
        if model.cameraAccess == .denied || model.cameraAccess == .restricted {
            cameraOff = true
            pill = "CAMERA OFF"
            pillColor = MG.clay
            title = "Camera access is off"
            detail = "\(DeviceInfo.displayName) needs the camera to send video to your PC. Turn it on in Settings."
        } else if let error = model.listenError {
            pill = "ERROR"
            pillColor = MG.rose
            title = "Couldn't start"
            detail = "Close \(DeviceInfo.displayName) and open it again. (\(error))"
        } else if model.isConnected, let reason = model.interruption {
            pill = "PAUSED"
            pillColor = MG.clay
            iris = 0.45
            (title, detail) = Self.pauseText(reason)
        } else if model.isConnected, let info = model.stream {
            pill = "LIVE"
            pillColor = MG.mangroveGlow
            iris = 1
            isLive = true
            stream = info
            title = "Streaming to your PC"
            detail = "You can leave the phone like this. Keep \(DeviceInfo.displayName) open on the screen."
        } else if model.isConnected {
            pill = "CONNECTED"
            pillColor = MG.tide
            iris = 0.32
            title = "Connected to your PC"
            detail = "Video starts as soon as the PC asks for it."
        } else if model.isListening {
            pill = "WAITING"
            pillColor = MG.inkMuted
            title = "Waiting for your PC"
            detail = "Plug this iPhone into your PC with a cable and open Cabled on Windows."
        }
    }

    static func pauseText(_ reason: String) -> (String, String) {
        switch reason {
        case "camera-in-use":
            return ("Camera in use by another app", "Close the other camera app and the video comes back by itself.")
        case "system-pressure":
            return ("Paused to cool down", "The iPhone got too hot for the camera. Video comes back by itself when it cools.")
        case "call":
            return ("Paused for a call", "Video comes back when the call ends.")
        case "background":
            return ("Paused in the background", "Open \(DeviceInfo.displayName) again to bring the video back.")
        case "multitasking":
            return ("Paused while another app is open", "Close the other app to bring the video back.")
        case "error":
            return ("The camera stopped", "Unplug the iPhone and plug it in again. If it keeps happening, send the log from Details.")
        default:
            return ("Camera paused", "Video comes back by itself.")
        }
    }

    static func heatWord(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal: return "Cool"
        case .fair: return "Warm"
        case .serious: return "Hot"
        case .critical: return "Too hot"
        @unknown default: return "Unknown"
        }
    }
}

/// Link test, the PC's version and the log, for debugging.
private struct DetailsSheet: View {
    @ObservedObject var model: CompanionModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section("PC") {
                    item("Connected", model.isConnected ? "Yes" : "No")
                    if let pc = model.pcDescription { item("App", pc) }
                    item("Pings answered", "\(model.pingCount)")
                }
                Section("Link test") {
                    item("Phone → PC", mbps(model.lastDownMbps))
                    item("PC → phone", mbps(model.lastUpMbps))
                }
                Section("App") {
                    item("Version", "\(DeviceInfo.appVersion) (\(DeviceInfo.buildNumber))")
                    item("Protocol", WireProtocol.versionString)
                    item("Signature expires", model.signingExpiry?.formatted(.relative(presentation: .named)) ?? "Unknown")
                }
                Section("Log") {
                    ForEach(Array(model.logLines.suffix(60).reversed().enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(MG.mono(11.5, .regular))
                            .foregroundStyle(MG.inkMuted)
                            .textSelection(.enabled)
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .listRowBackground(MG.surface200)
            .background(MG.surface100)
            .navigationTitle("Details")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
        .tint(MG.mangroveGlow)
        .presentationDetents([.medium, .large])
    }

    private func item(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(MG.text(14)).foregroundStyle(MG.inkMuted)
            Spacer()
            Text(value).font(MG.mono(13.5)).foregroundStyle(MG.ink).multilineTextAlignment(.trailing)
        }
        .listRowBackground(MG.surface200)
    }

    private func mbps(_ value: Double?) -> String {
        guard let value else { return "–" }
        return String(format: "%.0f Mbit/s", value)
    }
}

private extension View {
    /// One orchestrated entrance: blocks rise 8 pt and fade in over dur-move, 30 ms apart.
    func entrance(_ shown: Bool, step: Int) -> some View {
        modifier(Entrance(shown: shown, step: step))
    }
}

private struct Entrance: ViewModifier {
    let shown: Bool
    let step: Int
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func body(content: Content) -> some View {
        content
            .opacity(shown || reduceMotion ? 1 : 0)
            .offset(y: shown || reduceMotion ? 0 : 8)
            .animation(reduceMotion ? nil : MG.move.delay(Double(step) * 0.03), value: shown)
    }
}
