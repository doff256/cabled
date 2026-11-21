import AVFoundation
import Foundation

/// The phone's cameras as the PC sees them (CAPS, R6): virtual multi-lens devices (they switch
/// lenses by themselves while zooming), each physical lens, and the front camera. Zoom is reported
/// as device factors plus a `zoomScale` so the PC can show iPhone-style numbers ("1×" = main lens).
enum CameraCatalog {
    private static let backTypes: [AVCaptureDevice.DeviceType] = [
        .builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera,
        .builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera,
    ]
    private static let maxDisplayZoom = 15.0  // beyond this, digital zoom is just mush

    static func devices() -> [AVCaptureDevice] {
        let back = AVCaptureDevice.DiscoverySession(deviceTypes: backTypes, mediaType: .video, position: .back).devices
        let front = AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInTrueDepthCamera, .builtInWideAngleCamera],
                                                     mediaType: .video, position: .front).devices
        return back + front.prefix(1)
    }

    /// Rear main camera (D-26), preferably inside a virtual device so zoom can reach other lenses.
    static func defaultDevice() -> AVCaptureDevice? {
        for type in [AVCaptureDevice.DeviceType.builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera,
                     .builtInWideAngleCamera] {
            if let d = AVCaptureDevice.default(type, for: .video, position: .back) { return d }
        }
        return AVCaptureDevice.default(for: .video)
    }

    static func device(id: String?) -> AVCaptureDevice? {
        if let id, !id.isEmpty, let d = AVCaptureDevice(uniqueID: id) { return d }
        return defaultDevice()
    }

    static func typeName(_ d: AVCaptureDevice) -> String {
        if d.position == .front { return "front" }
        switch d.deviceType {
        case .builtInTripleCamera: return "triple"
        case .builtInDualWideCamera: return "dualWide"
        case .builtInDualCamera: return "dual"
        case .builtInUltraWideCamera: return "ultraWide"
        case .builtInTelephotoCamera: return "telephoto"
        default: return "wide"
        }
    }

    static func switchOver(_ d: AVCaptureDevice) -> [Double] {
        d.isVirtualDevice ? d.virtualDeviceSwitchOverVideoZoomFactors.map { $0.doubleValue } : []
    }

    private static func hasUltraWide(_ d: AVCaptureDevice) -> Bool {
        d.constituentDevices.contains { $0.deviceType == .builtInUltraWideCamera }
    }

    /// display zoom = device factor × zoomScale.
    static func zoomScale(_ d: AVCaptureDevice) -> Double {
        if d.position == .front { return 1 }
        if d.isVirtualDevice {
            // On devices that start at the ultra wide, factor 1 is "0.5×"; the main lens is the
            // first switch-over point.
            if hasUltraWide(d), let first = switchOver(d).first, first > 0 { return 1 / first }
            return 1
        }
        // Physical lenses: magnification relative to the rear main lens, from the fields of view.
        guard let main = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back),
              main.uniqueID != d.uniqueID else { return 1 }
        let mainFov = Double(main.activeFormat.videoFieldOfView) * .pi / 180
        let fov = Double(d.activeFormat.videoFieldOfView) * .pi / 180
        guard fov > 0, mainFov > 0 else { return 1 }
        let ratio = tan(mainFov / 2) / tan(fov / 2)
        return (ratio * 10).rounded() / 10  // 0.5, 2, 3, 4, 5...
    }

    /// Where a camera starts: the main lens for virtual devices that include the ultra wide.
    static func defaultZoom(_ d: AVCaptureDevice) -> Double {
        if d.isVirtualDevice, hasUltraWide(d), let first = switchOver(d).first { return first }
        return Double(d.minAvailableVideoZoomFactor)
    }

    static func maxZoom(_ d: AVCaptureDevice) -> Double {
        let hardware = min(Double(d.maxAvailableVideoZoomFactor), Double(d.activeFormat.videoMaxZoomFactor))
        return max(Double(d.minAvailableVideoZoomFactor), min(hardware, maxDisplayZoom / zoomScale(d)))
    }

    /// The physical lens in use (virtual devices switch while zooming).
    static func lensName(_ d: AVCaptureDevice) -> String {
        if d.isVirtualDevice, let active = d.activePrimaryConstituent { return typeName(active) }
        return typeName(d)
    }

    /// 420v formats ≥ 1280 wide, deduplicated by size/binning (highest frame rate kept).
    static func formats(_ d: AVCaptureDevice) -> [[String: Any]] {
        var formats: [[String: Any]] = []
        var seen: [String: Int] = [:]
        for format in d.formats {
            guard CMFormatDescriptionGetMediaSubType(format.formatDescription)
                    == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange else { continue }
            let dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            guard dims.width >= 1280 else { continue }
            let maxFps = format.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 30
            let minFps = format.videoSupportedFrameRateRanges.map(\.minFrameRate).min() ?? 1
            let key = "\(dims.width)x\(dims.height)/\(format.isVideoBinned)"
            if let i = seen[key] {
                if maxFps > (formats[i]["maxFps"] as? Double ?? 0) { formats[i]["maxFps"] = maxFps }
                continue
            }
            seen[key] = formats.count
            formats.append(["width": Int(dims.width), "height": Int(dims.height), "minFps": minFps,
                            "maxFps": maxFps, "binned": format.isVideoBinned])
        }
        return formats
    }

    static func capabilities() -> [String: Any] {
        let all = devices()
        let cameras: [[String: Any]] = all.map { d in
            [
                "id": d.uniqueID,
                "name": d.localizedName,
                "position": d.position == .front ? "front" : "back",
                "type": typeName(d),
                "virtual": d.isVirtualDevice,
                "minZoom": Double(d.minAvailableVideoZoomFactor),
                "maxZoom": maxZoom(d),
                "zoomScale": zoomScale(d),
                "defaultZoom": defaultZoom(d),
                "switchOver": switchOver(d),
                "formats": formats(d),
            ]
        }
        let defaultCamera = defaultDevice()
        return [
            "codecs": ["h264", "hevc"],
            "defaultCamera": defaultCamera?.uniqueID ?? "",
            "formats": defaultCamera.map { formats($0) } ?? [],  // pre-R6 PCs read only this
            "cameras": cameras,
        ]
    }
}
