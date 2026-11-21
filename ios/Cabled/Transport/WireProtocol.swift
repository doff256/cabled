import Foundation

/// Cabled wire protocol (docs/transport-and-protocol.md section 2).
/// Wire format: u32 length (of everything after this field) | u16 type | u16 flags | payload, little-endian.
/// Must stay byte-identical to windows/src/common/protocol.{h,cpp}.
enum WireProtocol {
    /// Major 0 = unstable spike protocol: minor versions must match exactly.
    static let versionMajor = 0
    static let versionMinor = 1
    static var versionString: String { "\(versionMajor).\(versionMinor)" }
    static let defaultPort: UInt16 = 47800
    static let maxBodyLength: UInt32 = 32 << 20
    static let headerSize = 8
    static let minProbeChunk = 8

    static func isCompatible(_ peerVersion: String) -> Bool {
        let parts = peerVersion.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 2, let major = Int(parts[0]), let minor = Int(parts[1]) else { return false }
        if versionMajor == 0 { return major == 0 && minor == versionMinor }
        return major == versionMajor
    }
}

enum MessageType: UInt16 {
    case hello = 0x0001
    case bye = 0x0002
    case error = 0x0003
    case ping = 0x0004
    case pong = 0x0005
    case log = 0x0006
    case status = 0x0007
    case probeRequest = 0x0010
    case probeData = 0x0011
    case probeEnd = 0x0012
    case probeResult = 0x0013
    case caps = 0x0020
    case configure = 0x0021
    case configured = 0x0022
    case video = 0x0030
    case keyframeRequest = 0x0031
    case controlSet = 0x0040
    case controlState = 0x0041
}

/// VIDEO payload (docs/transport-and-protocol.md): 16-byte little-endian header
/// (u64 ptsNs, u32 frameSeq, u8 codec, u8 flags, u16 metaLength), metadata, then AVCC NAL units.
enum VideoMessage {
    static let headerSize = 16
    static let codecH264: UInt8 = 1
    static let codecHEVC: UInt8 = 2
    static let flagKeyframe: UInt8 = 1 << 0
    static let flagDiscontinuity: UInt8 = 1 << 2

    static func encodeHeader(ptsNs: UInt64, frameSeq: UInt32, codec: UInt8, flags: UInt8, metaLength: UInt16) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: headerSize)
        WireCodec.writeLE64(&out, at: 0, ptsNs)
        for i in 0..<4 { out[8 + i] = UInt8(truncatingIfNeeded: frameSeq >> (8 * UInt32(i))) }
        out[12] = codec
        out[13] = flags
        out[14] = UInt8(truncatingIfNeeded: metaLength)
        out[15] = UInt8(truncatingIfNeeded: metaLength >> 8)
        return out
    }

    /// A complete wire message (8-byte message header + VIDEO payload) in one buffer.
    static func message(ptsNs: UInt64, frameSeq: UInt32, flags: UInt8, avcc: [UInt8],
                        codec: UInt8 = VideoMessage.codecH264) -> [UInt8] {
        let header = encodeHeader(ptsNs: ptsNs, frameSeq: frameSeq, codec: codec, flags: flags, metaLength: 0)
        var out = [UInt8](repeating: 0, count: WireProtocol.headerSize)
        out.reserveCapacity(WireProtocol.headerSize + headerSize + avcc.count)
        WireCodec.writeHeader(into: &out, type: .video, payloadCount: headerSize + avcc.count)
        out += header
        out += avcc
        return out
    }
}

struct WireMessage {
    var rawType: UInt16
    var flags: UInt16
    var payload: [UInt8]

    var type: MessageType? { MessageType(rawValue: rawType) }
    var text: String { String(decoding: payload, as: UTF8.self) }
}

enum WireCodec {
    /// Writes the 8-byte header at the start of `buffer`.
    static func writeHeader(into buffer: inout [UInt8], type: MessageType, flags: UInt16 = 0, payloadCount: Int) {
        precondition(buffer.count >= WireProtocol.headerSize)
        let length = UInt32(4 + payloadCount)
        buffer.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: length.littleEndian, toByteOffset: 0, as: UInt32.self)
            raw.storeBytes(of: type.rawValue.littleEndian, toByteOffset: 4, as: UInt16.self)
            raw.storeBytes(of: flags.littleEndian, toByteOffset: 6, as: UInt16.self)
        }
    }

    static func encode(_ type: MessageType, flags: UInt16 = 0, payload: [UInt8] = []) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: WireProtocol.headerSize + payload.count)
        writeHeader(into: &out, type: type, flags: flags, payloadCount: payload.count)
        if !payload.isEmpty {
            out.replaceSubrange(WireProtocol.headerSize..<out.count, with: payload)
        }
        return out
    }

    /// Reads the u32 length that starts every message.
    static func decodeLength(_ bytes: [UInt8]) -> UInt32 {
        precondition(bytes.count >= 4)
        return UInt32(bytes[0]) | UInt32(bytes[1]) << 8 | UInt32(bytes[2]) << 16 | UInt32(bytes[3]) << 24
    }

    /// Decodes what follows the length field: u16 type | u16 flags | payload.
    static func decodeBody(_ body: [UInt8]) -> WireMessage? {
        guard body.count >= 4 else { return nil }
        let rawType = UInt16(body[0]) | UInt16(body[1]) << 8
        let flags = UInt16(body[2]) | UInt16(body[3]) << 8
        return WireMessage(rawType: rawType, flags: flags, payload: Array(body[4...]))
    }

    /// Decodes one complete message from the front of `bytes`. Returns it and the bytes consumed.
    static func decode(_ bytes: [UInt8]) -> (WireMessage, Int)? {
        guard bytes.count >= 4 else { return nil }
        let length = Int(decodeLength(bytes))
        guard length >= 4, bytes.count >= 4 + length,
              let message = decodeBody(Array(bytes[4..<(4 + length)])) else { return nil }
        return (message, 4 + length)
    }

    // MARK: JSON bodies

    static func json(_ object: [String: Any]) -> [UInt8] {
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return [] }
        return [UInt8](data)
    }

    static func parseJSON(_ payload: [UInt8]) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: Data(payload))) as? [String: Any]
    }

    // MARK: Ping / pong

    static func decodePing(_ payload: [UInt8]) -> UInt64? {
        guard payload.count >= 8 else { return nil }
        return readLE64(payload, at: 0)
    }

    static func encodePong(pcTimeNs: UInt64, phoneTimeNs: UInt64) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: 16)
        writeLE64(&out, at: 0, pcTimeNs)
        writeLE64(&out, at: 8, phoneTimeNs)
        return out
    }

    // MARK: Probe chunks — u64 sequence, then byte[i] = i & 0xFF for i >= 8

    static func fillProbePattern(_ buffer: inout [UInt8], payloadOffset: Int) {
        let count = buffer.count - payloadOffset
        guard count > WireProtocol.minProbeChunk else { return }
        for i in WireProtocol.minProbeChunk..<count {
            buffer[payloadOffset + i] = UInt8(truncatingIfNeeded: i)
        }
    }

    static func writeProbeSequence(_ buffer: inout [UInt8], payloadOffset: Int, seq: UInt64) {
        writeLE64(&buffer, at: payloadOffset, seq)
    }

    static func verifyProbeChunk(_ payload: [UInt8]) -> UInt64? {
        guard payload.count >= WireProtocol.minProbeChunk else { return nil }
        for i in WireProtocol.minProbeChunk..<payload.count where payload[i] != UInt8(truncatingIfNeeded: i) {
            return nil
        }
        return readLE64(payload, at: 0)
    }

    // MARK: Little-endian helpers

    static func writeLE64(_ buffer: inout [UInt8], at offset: Int, _ value: UInt64) {
        for i in 0..<8 { buffer[offset + i] = UInt8(truncatingIfNeeded: value >> (8 * UInt64(i))) }
    }

    static func readLE64(_ buffer: [UInt8], at offset: Int) -> UInt64 {
        var value: UInt64 = 0
        for i in (0..<8).reversed() { value = value << 8 | UInt64(buffer[offset + i]) }
        return value
    }
}

/// Monotonic clock in nanoseconds for PONG timestamps and probe timing. CLOCK_UPTIME_RAW is
/// mach_absolute_time, the same clock camera sample buffers are stamped with (host time clock), so
/// VIDEO ptsNs and PONG phoneTimeNs are directly comparable on the PC.
func monotonicNanos() -> UInt64 {
    clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
}
