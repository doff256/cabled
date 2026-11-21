import CoreMedia
import Foundation
import VideoToolbox

/// Hardware H.264 / HEVC encoder (docs/ios-companion.md, Encoder settings): real time, no frame
/// reordering, low-latency rate control where available, keyframe every 2 s and on request, live
/// bitrate changes (R5 adaptation). Output is AVCC/HVCC-style: 4-byte big-endian NAL lengths, with
/// the parameter sets (VPS/SPS/PPS) prepended on every keyframe.
final class VideoEncoder {
    typealias Output = (_ data: [UInt8], _ keyframe: Bool, _ ptsNs: UInt64) -> Void

    struct EncoderError: Error, CustomStringConvertible {
        let what: String
        let status: OSStatus
        var description: String { "\(what) failed (\(status))" }
    }

    let hevc: Bool
    private var session: VTCompressionSession?
    private let output: Output
    private let lock = NSLock()
    private var forceKeyframe = true  // the first frame is always a keyframe
    private(set) var lowLatencyRateControl = false

    init(hevc: Bool, width: Int32, height: Int32, fps: Double, bitrate: Int, output: @escaping Output) throws {
        self.hevc = hevc
        self.output = output
        let codec = hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264

        var created: VTCompressionSession?
        let lowLatency = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        var status = VTCompressionSessionCreate(allocator: kCFAllocatorDefault, width: width, height: height,
                                                codecType: codec, encoderSpecification: lowLatency,
                                                imageBufferAttributes: nil, compressedDataAllocator: nil,
                                                outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        if status == noErr {
            lowLatencyRateControl = true
        } else {
            // Default encoder: hardware on every iPhone; still real time, no reordering.
            // (RequireHardwareAcceleratedVideoEncoder would need iOS 17.4; our minimum is 17.0.)
            status = VTCompressionSessionCreate(allocator: kCFAllocatorDefault, width: width, height: height,
                                                codecType: codec, encoderSpecification: nil,
                                                imageBufferAttributes: nil, compressedDataAllocator: nil,
                                                outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        }
        guard status == noErr, let session = created else {
            throw EncoderError(what: "VTCompressionSessionCreate(\(hevc ? "HEVC" : "H.264"))", status: status)
        }
        self.session = session

        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel,
            hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2.0))
        // Tag the stream BT.709 like the capture (docs/video-pipeline.md, Colour).
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        setBitrate(bitrate)
        VTCompressionSessionPrepareToEncodeFrames(session)
    }

    deinit { invalidate() }

    private func set(_ key: CFString, _ value: CFTypeRef) {
        guard let session else { return }
        let s = VTSessionSetProperty(session, key: key, value: value)
        if s != noErr { NSLog("Cabled: encoder property %@ not accepted (%d)", key as String, s) }
    }

    /// Takes effect within a few frames.
    func setBitrate(_ bitrate: Int) {
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        set(kVTCompressionPropertyKey_DataRateLimits,
            [NSNumber(value: Double(bitrate) * 1.5 / 8.0), NSNumber(value: 1.0)] as CFArray)
    }

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

        var data: [UInt8] = []
        if keyframe, let format = CMSampleBufferGetFormatDescription(sampleBuffer) {
            data += hevc ? Self.hevcParameterSets(format) : Self.h264ParameterSets(format)
        }
        let length = CMBlockBufferGetDataLength(block)
        let start = data.count
        data += [UInt8](repeating: 0, count: length)
        let copied = data.withUnsafeMutableBytes { raw in
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: raw.baseAddress! + start)
        }
        guard copied == kCMBlockBufferNoErr else { return }
        output(data, keyframe, ptsNs)
    }

    private static func lengthPrefixed(_ pointer: UnsafePointer<UInt8>, _ size: Int) -> [UInt8] {
        let n = UInt32(size)
        return [UInt8(n >> 24), UInt8((n >> 16) & 0xFF), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF)]
            + Array(UnsafeBufferPointer(start: pointer, count: size))
    }

    private static func h264ParameterSets(_ format: CMFormatDescription) -> [UInt8] {
        var count = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                 parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                                 nalUnitHeaderLengthOut: nil) == noErr else { return [] }
        var out: [UInt8] = []
        for index in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index,
                                                                  parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                                                                  parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
               let pointer {
                out += lengthPrefixed(pointer, size)
            }
        }
        return out
    }

    private static func hevcParameterSets(_ format: CMFormatDescription) -> [UInt8] {
        var count = 0
        guard CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                 parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                                 nalUnitHeaderLengthOut: nil) == noErr else { return [] }
        var out: [UInt8] = []
        for index in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            if CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: index,
                                                                  parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                                                                  parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
               let pointer {
                out += lengthPrefixed(pointer, size)
            }
        }
        return out
    }
}
