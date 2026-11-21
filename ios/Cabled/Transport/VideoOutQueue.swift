import Foundation

/// Encoded frames waiting to go to the PC (docs/transport-and-protocol.md, flow control).
/// At most 2 frames wait. On a stall the queue is dropped and frames are skipped until the next
/// keyframe, which is requested at once. Frames are numbered in encode order, so anything dropped
/// shows up as a gap on the PC, which then also waits for the keyframe instead of decoding garbage.
final class VideoOutQueue {
    private let condition = NSCondition()
    private var pending: [[UInt8]] = []
    private var stopped = false
    private var droppingUntilKeyframe = false
    private var seq: UInt32 = 0
    private let send: ([UInt8]) -> Bool
    private let onStall: () -> Void
    private var codec = VideoMessage.codecH264
    private static let maxPending = 2

    init(send: @escaping ([UInt8]) -> Bool, onStall: @escaping () -> Void) {
        self.send = send
        self.onStall = onStall
    }

    /// Set once the encoder is known (it may fall back from HEVC to H.264).
    func setCodec(hevc: Bool) {
        condition.lock()
        codec = hevc ? VideoMessage.codecHEVC : VideoMessage.codecH264
        condition.unlock()
    }

    func start() {
        let thread = Thread { [self] in run() }
        thread.name = "cabled.video-out"
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    func stop() {
        condition.lock()
        stopped = true
        pending.removeAll()
        condition.broadcast()
        condition.unlock()
    }

    /// Called from the encoder's output thread.
    func push(avcc: [UInt8], keyframe: Bool, ptsNs: UInt64) {
        var stalled = false
        condition.lock()
        defer {
            condition.unlock()
            if stalled { onStall() }
        }
        guard !stopped else { return }
        seq &+= 1
        if droppingUntilKeyframe && !keyframe { return }
        if pending.count >= Self.maxPending {
            pending.removeAll()  // the link can't keep up: skip to fresh video
            if !keyframe {
                droppingUntilKeyframe = true
                stalled = true
                return
            }
        }
        if keyframe { droppingUntilKeyframe = false }
        pending.append(VideoMessage.message(ptsNs: ptsNs, frameSeq: seq,
                                            flags: keyframe ? VideoMessage.flagKeyframe : 0, avcc: avcc,
                                            codec: codec))
        condition.signal()
    }

    private func run() {
        while true {
            condition.lock()
            while pending.isEmpty && !stopped { condition.wait() }
            if stopped {
                condition.unlock()
                return
            }
            let message = pending.removeFirst()
            condition.unlock()
            if !send(message) {
                stop()
                return
            }
        }
    }
}
