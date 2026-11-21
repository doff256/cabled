import CoreMedia
import Foundation
import VideoToolbox

/// Hardware H.264 encoder (docs/ios-companion.md, Encoder settings): real time, no frame reordering,
/// low-latency rate control where available, keyframe every 2 s and on request. Output is AVCC
/// (4-byte big-endian NAL lengths) with SPS/PPS prepended on every keyframe.
final class H264Encoder {
    typealias Output = (_ avcc: [UInt8], _ keyframe: Bool, _ ptsNs: UInt64) -> Void

    struct EncoderError: Error, CustomStringConvertible {
        let what: String
        let status: OSStatus
        var description: String { "\(what) failed (\(status))" }
    }

    private var session: VTCompressionSession?
    private let output: Output
    private let lock = NSLock()
    private var forceKeyframe = true  // the first frame is always a keyframe
    private(set) var lowLatencyRateControl = false

    init(width: Int32, height: Int32, fps: Double, bitrate: Int, output: @escaping Output) throws {
        self.output = output

        var created: VTCompressionSession?
        let lowLatency = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        var status = VTCompressionSessionCreate(allocator: kCFAllocatorDefault, width: width, height: height,
                                                codecType: kCMVideoCodecType_H264, encoderSpecification: lowLatency,
                                                imageBufferAttributes: nil, compressedDataAllocator: nil,
                                                outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        if status == noErr {
            lowLatencyRateControl = true
        } else {
            // Fall back to the default encoder (hardware on every iPhone; still real time, no reordering).
            // RequireHardwareAcceleratedVideoEncoder would need iOS 17.4; our minimum is 17.0.
            status = VTCompressionSessionCreate(allocator: kCFAllocatorDefault, width: width, height: height,
                                                codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                                                imageBufferAttributes: nil, compressedDataAllocator: nil,
                                                outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        }
        guard status == noErr, let session = created else {
            throw EncoderError(what: "VTCompressionSessionCreate", status: status)
        }
        self.session = session

        func set(_ key: CFString, _ value: CFTypeRef) {
            let s = VTSessionSetProperty(session, key: key, value: value)
            if s != noErr { NSLog("Cabled: encoder property %@ not accepted (%d)", key as String, s) }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        set(kVTCompressionPropertyKey_DataRateLimits, [NSNumber(value: Double(bitrate) * 1.5 / 8.0), NSNumber(value: 1.0)] as CFArray)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2.0))
        // Tag the stream BT.709 like the capture (docs/video-pipeline.md, Colour).
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTCompressionSessionPrepareToEncodeFrames(session)
    }

    deinit { invalidate() }

    func requestKeyframe() {
        lock.lock()
        forceKeyframe = true
        lock.unlock()
    }

    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime) {
        guard let session else { return }
        lock.lock()
        let keyframe = forceKeyframe
        forceKeyframe = false
        lock.unlock()
        let properties = keyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary : nil
        let ptsNs = UInt64(max(0, CMTimeGetSeconds(pts)) * 1e9)  // host clock = mach_absolute_time
        VTCompressionSessionEncodeFrame(session, imageBuffer: pixelBuffer, presentationTimeStamp: pts,
                                        duration: .invalid, frameProperties: properties, infoFlagsOut: nil) {
            [weak self] status, _, sampleBuffer in
            guard status == noErr, let sampleBuffer, let self else { return }
            self.deliver(sampleBuffer, ptsNs: ptsNs)
        }
    }

    func invalidate() {
        guard let session else { return }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        self.session = nil
    }

    // MARK: Output

    private func deliver(_ sampleBuffer: CMSampleBuffer, ptsNs: UInt64) {
        var keyframe = true
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[String: Any]],
           let first = attachments.first {
            keyframe = !((first[kCMSampleAttachmentKey_NotSync as String] as? Bool) ?? false)
        }
        guard let block = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }

        var avcc: [UInt8] = []
        if keyframe, let format = CMSampleBufferGetFormatDescription(sampleBuffer) {
            avcc += Self.parameterSets(format)
        }
        let length = CMBlockBufferGetDataLength(block)
        let start = avcc.count
        avcc += [UInt8](repeating: 0, count: length)
        let copied = avcc.withUnsafeMutableBytes { raw in
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: raw.baseAddress! + start)
        }
        guard copied == kCMBlockBufferNoErr else { return }
        output(avcc, keyframe, ptsNs)
    }

    /// SPS and PPS as AVCC NAL units (4-byte big-endian length + bytes).
    private static func parameterSets(_ format: CMFormatDescription) -> [UInt8] {
        var count = 0
        var headerLength: Int32 = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                 parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                                 nalUnitHeaderLengthOut: &headerLength) == noErr else {
            return []
        }
        var out: [UInt8] = []
        for index in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index,
                                                                     parameterSetPointerOut: &pointer,
                                                                     parameterSetSizeOut: &size, parameterSetCountOut: nil,
                                                                     nalUnitHeaderLengthOut: nil) == noErr,
                  let pointer else { continue }
            let n = UInt32(size)
            out += [UInt8(n >> 24), UInt8((n >> 16) & 0xFF), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF)]
            out += UnsafeBufferPointer(start: pointer, count: size)
        }
        return out
    }
}
