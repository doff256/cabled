import SwiftUI

/// The app icon's framing iris, alive: four blades close onto a square opening with a subject dot inside.
/// `open` 0 is shut, 1 is wide open. Changes land with ease-spring; while `breathing`, the blades drift
/// slowly (dur-breathe). Under reduced motion it holds still.
struct IrisView: View {
    var open: Double
    var breathing: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        let breathes = breathing && !reduceMotion
        TimelineView(.animation(minimumInterval: 1.0 / 30, paused: !breathes)) { context in
            let drift = breathes
                ? 0.06 * sin(context.date.timeIntervalSinceReferenceDate * 2 * .pi / MG.breathePeriod) : 0
            ZStack {
                Circle().fill(MG.lacquer).scaleEffect(0.76)
                Circle().fill(MG.onMangrove).scaleEffect(0.1)
                    .opacity(open > 0.3 ? 1 : 0)
                IrisBlades(open: open, drift: drift, parity: 1).fill(MG.onMangrove.opacity(0.6))
                IrisBlades(open: open, drift: drift, parity: 0).fill(MG.onMangrove)
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .animation(reduceMotion ? nil : MG.settle, value: open)
        .accessibilityHidden(true)
    }
}

/// Half of the iris's blades (even or odd), on a 100×100 grid: a pinwheel between an inner square
/// (the opening) and an outer circle. Same geometry as design/icon/make-icon.mjs.
struct IrisBlades: Shape {
    var open: Double
    var drift: Double
    var parity: Int

    var animatableData: Double {
        get { open }
        set { open = newValue }
    }

    func path(in rect: CGRect) -> Path {
        let s = min(rect.width, rect.height) / 100
        let cx = rect.midX, cy = rect.midY
        let o = max(0.04, min(1.08, open + drift))
        let n = 4, bigR = 36.0, r = 4 + 17 * o, rot = Double.pi / 4

        var v: [(x: Double, y: Double)] = []
        for i in 0..<n {
            let a = rot + Double(i) * 2 * .pi / Double(n)
            v.append((r * cos(a), r * sin(a)))
        }
        // Each edge of the opening, carried on to the outer circle.
        var ends: [(x: Double, y: Double)] = []
        for i in 0..<n {
            let q = v[i], p = v[(i + 1) % n]
            var dx = p.x - q.x, dy = p.y - q.y
            let len = (dx * dx + dy * dy).squareRoot()
            dx /= len
            dy /= len
            let pd = p.x * dx + p.y * dy
            let t = -pd + (pd * pd - (p.x * p.x + p.y * p.y) + bigR * bigR).squareRoot()
            ends.append((p.x + t * dx, p.y + t * dy))
        }
        func pt(_ a: (x: Double, y: Double)) -> CGPoint { CGPoint(x: cx + a.x * s, y: cy + a.y * s) }

        var path = Path()
        for i in 0..<n where i % 2 == parity {
            let back = ends[(i + n - 1) % n]
            path.move(to: pt(v[i]))
            path.addLine(to: pt(v[(i + 1) % n]))
            path.addLine(to: pt(ends[i]))
            // Along the outer circle back to where the previous edge meets it.
            let a0 = atan2(ends[i].y, ends[i].x)
            var a1 = atan2(back.y, back.x)
            while a1 > a0 { a1 -= 2 * .pi }
            for k in 1...16 {
                let a = a0 + (a1 - a0) * Double(k) / 16
                path.addLine(to: pt((bigR * cos(a), bigR * sin(a))))
            }
            path.closeSubpath()
        }
        return path
    }
}
