@testable import Companion
import XCTest

final class WireProtocolTests: XCTestCase {
    /// Same bytes as Protocol.HeaderLayoutMatchesSpec in windows/tests/protocol_tests.cpp.
    func testHeaderLayoutMatchesWindows() {
        let bytes = WireCodec.encode(.ping, flags: 0x0102, payload: [0xAA, 0xBB, 0xCC])
        XCTAssertEqual(bytes, [7, 0, 0, 0, 0x04, 0x00, 0x02, 0x01, 0xAA, 0xBB, 0xCC])
    }

    func testDecodeRoundTrip() throws {
        let payload = WireCodec.json(["protocol": "0.1", "role": "phone"])
        var stream = WireCodec.encode(.hello, payload: payload)
        stream += WireCodec.encode(.log, payload: Array("hi".utf8))

        let (first, used) = try XCTUnwrap(WireCodec.decode(stream))
        XCTAssertEqual(first.type, .hello)
        XCTAssertEqual(WireCodec.parseJSON(first.payload)?["role"] as? String, "phone")

        let (second, _) = try XCTUnwrap(WireCodec.decode(Array(stream[used...])))
        XCTAssertEqual(second.type, .log)
        XCTAssertEqual(second.text, "hi")

        XCTAssertNil(WireCodec.decode(Array(stream.prefix(5))))  // incomplete
    }

    func testVersionCompatibility() {
        XCTAssertEqual(WireProtocol.versionString, "0.1")
        XCTAssertTrue(WireProtocol.isCompatible("0.1"))
        XCTAssertFalse(WireProtocol.isCompatible("0.2"))
        XCTAssertFalse(WireProtocol.isCompatible("1.1"))
        XCTAssertFalse(WireProtocol.isCompatible("nonsense"))
        XCTAssertFalse(WireProtocol.isCompatible(""))
    }

    func testProbePatternMatchesWindows() throws {
        let header = WireProtocol.headerSize
        var buffer = [UInt8](repeating: 0, count: header + 300)
        WireCodec.writeHeader(into: &buffer, type: .probeData, payloadCount: 300)
        WireCodec.fillProbePattern(&buffer, payloadOffset: header)
        WireCodec.writeProbeSequence(&buffer, payloadOffset: header, seq: 0x0102030405060708)

        let payload = Array(buffer[header...])
        XCTAssertEqual(payload[0], 0x08)  // little-endian sequence
        XCTAssertEqual(payload[8], 8)
        XCTAssertEqual(payload[256], 0)  // wraps at 256
        XCTAssertEqual(WireCodec.verifyProbeChunk(payload), 0x0102030405060708)

        var corrupted = payload
        corrupted[200] ^= 0xFF
        XCTAssertNil(WireCodec.verifyProbeChunk(corrupted))
    }

    /// Same vector as Video.HeaderRoundTripWithMetadata in windows/tests/video_tests.cpp.
    func testVideoHeaderMatchesWindows() {
        let header = VideoMessage.encodeHeader(ptsNs: 0x1122334455667788, frameSeq: 42, codec: 1,
                                               flags: VideoMessage.flagKeyframe | VideoMessage.flagDiscontinuity,
                                               metaLength: 3)
        XCTAssertEqual(header, [0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 42, 0, 0, 0, 1, 5, 3, 0])
    }

    func testVideoMessageFraming() throws {
        let avcc: [UInt8] = [0, 0, 0, 1, 0x65]
        let bytes = VideoMessage.message(ptsNs: 7, frameSeq: 9, flags: VideoMessage.flagKeyframe, avcc: avcc)
        let (message, used) = try XCTUnwrap(WireCodec.decode(bytes))
        XCTAssertEqual(used, bytes.count)
        XCTAssertEqual(message.type, .video)
        XCTAssertEqual(message.payload.count, VideoMessage.headerSize + avcc.count)
        XCTAssertEqual(message.payload[8], 9)                  // frameSeq
        XCTAssertEqual(message.payload[13], 1)                 // keyframe flag
        XCTAssertEqual(Array(message.payload[16...]), avcc)
        XCTAssertEqual(MessageType.configure.rawValue, 0x0021)
        XCTAssertEqual(MessageType.keyframeRequest.rawValue, 0x0031)
    }

    func testPingPong() {
        var ping = [UInt8](repeating: 0, count: 8)
        WireCodec.writeLE64(&ping, at: 0, 123_456_789)
        XCTAssertEqual(WireCodec.decodePing(ping), 123_456_789)
        XCTAssertNil(WireCodec.decodePing([1, 2, 3]))

        let pong = WireCodec.encodePong(pcTimeNs: 1, phoneTimeNs: 2)
        XCTAssertEqual(pong.count, 16)
        XCTAssertEqual(WireCodec.readLE64(pong, at: 0), 1)
        XCTAssertEqual(WireCodec.readLE64(pong, at: 8), 2)
    }
}
