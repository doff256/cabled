import CoreText
import SwiftUI

/// Mangrove Glass, the owner's design system (https://github.com/doff256/mangrove-glass): the dark tokens,
/// the three type faces and the few materials this one screen uses. Glass itself is Apple's Liquid Glass
/// on iOS 26 (system material on older versions), never drawn by hand.
enum MG {
    static let surface000 = Color(hex: 0x0B0C0D)
    static let surface100 = Color(hex: 0x131415)
    static let surface200 = Color(hex: 0x1A1B1D)
    static let surface300 = Color(hex: 0x232527)
    static let line = Color(hex: 0x2A2C2F)
    static let ink = Color(hex: 0xECE8E4)
    static let inkMuted = Color(hex: 0xA39D97)
    static let inkFaint = Color(hex: 0x6B6661)
    static let mangrove = Color(hex: 0x8E3B33)
    static let mangroveDeep = Color(hex: 0x4B1F1B)
    static let mangroveGlow = Color(hex: 0xD9735F)
    static let onMangrove = Color(hex: 0xF7EBE8)
    static let tide = Color(hex: 0x5FA8B8)
    static let clay = Color(hex: 0xCF8452)
    static let rose = Color(hex: 0xE0697C)
    /// The app icon's lacquer, top to bottom.
    static let lacquer = LinearGradient(colors: [Color(hex: 0xA8493E), Color(hex: 0x5A241F)], startPoint: .top, endPoint: .bottom)

    // Bricolage Grotesque for titles, Geist for text, Geist Mono for every number. All scale with Dynamic Type.
    static func display(_ size: CGFloat) -> Font {
        .custom("Bricolage Grotesque", size: size, relativeTo: .title).weight(.semibold)
    }
    static func text(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .custom("Geist", size: size, relativeTo: .body).weight(weight)
    }
    static func mono(_ size: CGFloat, _ weight: Font.Weight = .medium) -> Font {
        .custom("Geist Mono", size: size, relativeTo: .body).weight(weight)
    }

    // Motion tokens.
    /// ease-spring over dur-settle: things that land (the iris, thumbs) overshoot slightly.
    static let settle = Animation.timingCurve(0.2, 0.9, 0.25, 1.15, duration: 0.52)
    /// ease-out over dur-move: values and panels that arrive.
    static let move = Animation.timingCurve(0.16, 1, 0.3, 1, duration: 0.26)
    static let breathePeriod = 4.8  // dur-breathe

    /// The fonts ship in the app bundle (OFL, licences beside them). Registered once at launch.
    static func registerFonts() {
        for name in ["BricolageGrotesque", "Geist", "GeistMono"] {
            guard let url = Bundle.main.url(forResource: name, withExtension: "ttf") else { continue }
            CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
        }
    }
}

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255,
                  opacity: opacity)
    }
}

extension View {
    /// Liquid Glass on iOS 26, the system's thin material before that.
    @ViewBuilder func mgGlass<S: Shape>(in shape: S) -> some View {
        if #available(iOS 26.0, *) {
            self.glassEffect(.regular, in: shape)
        } else {
            self.background(.ultraThinMaterial, in: shape)
        }
    }

    /// A carved well (surface-000 pressed into the panel): the screen's hero sits in one.
    func mgWell(cornerRadius: CGFloat = 22) -> some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        return self.background {
            shape.fill(MG.surface000.shadow(.inner(color: .black.opacity(0.65), radius: 6, x: 0, y: 2)))
                .overlay(shape.strokeBorder(LinearGradient(colors: [.clear, .white.opacity(0.05)],
                                                           startPoint: .top, endPoint: .bottom), lineWidth: 1))
        }
    }

    /// A solid card (surface-200 with a hairline).
    func mgCard(edge: Color = MG.line) -> some View {
        let shape = RoundedRectangle(cornerRadius: 14, style: .continuous)
        return self.background(shape.fill(MG.surface200)).overlay(shape.strokeBorder(edge, lineWidth: 1))
    }
}

/// The primary button: lacquered mangrove with the glass highlight on its top half. One per view.
struct LacquerButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(MG.text(14, .semibold))
            .foregroundStyle(MG.onMangrove)
            .padding(.horizontal, 16)
            .padding(.vertical, 9)
            .background {
                Capsule().fill(LinearGradient(colors: [MG.mangrove, MG.mangroveDeep], startPoint: .top, endPoint: .bottom))
                    .overlay(Capsule().fill(LinearGradient(stops: [.init(color: .white.opacity(0.18), location: 0),
                                                                    .init(color: .white.opacity(0.04), location: 0.48),
                                                                    .init(color: .clear, location: 0.48)],
                                                           startPoint: .top, endPoint: .bottom)))
                    .overlay(Capsule().strokeBorder(MG.mangroveDeep, lineWidth: 1))
            }
            .brightness(configuration.isPressed ? -0.06 : 0)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)  // dur-snap
    }
}

/// A quiet button that sits on glass.
struct GhostButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(MG.text(14, .semibold))
            .foregroundStyle(MG.ink)
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .background(Capsule().fill(.white.opacity(configuration.isPressed ? 0.14 : 0.08)))
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// The status pill: the word and the dot take the state's colour. A live dot breathes.
struct StatusPill: View {
    let word: String
    let color: Color
    let breathing: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 6, height: 6)
                .modifier(Breathing(active: breathing && !reduceMotion))
            Text(word).font(MG.text(11, .semibold)).tracking(1.3)
        }
        .foregroundStyle(color)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(Capsule().fill(MG.surface200))
        .overlay(Capsule().strokeBorder(MG.line, lineWidth: 1))
        .accessibilityElement(children: .combine)
    }
}

private struct Breathing: ViewModifier {
    let active: Bool
    func body(content: Content) -> some View {
        if active {
            content.phaseAnimator([1.0, 0.35]) { view, opacity in view.opacity(opacity) } animation: { _ in
                .easeInOut(duration: MG.breathePeriod / 2)
            }
        } else {
            content
        }
    }
}
