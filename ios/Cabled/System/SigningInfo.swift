import Foundation

/// Reads when the app's free-account signature runs out (7 days, R-22).
/// Sideloaded and Xcode-installed apps carry embedded.mobileprovision: a CMS envelope around an XML
/// plist. We don't need to verify the signature, only read ExpirationDate, so we cut the plist out.
enum SigningInfo {
    static func expirationDate(bundle: Bundle = .main) -> Date? {
        guard let url = bundle.url(forResource: "embedded", withExtension: "mobileprovision"),
              let data = try? Data(contentsOf: url) else { return nil }
        return expirationDate(fromProvision: data)
    }

    static func expirationDate(fromProvision data: Data) -> Date? {
        guard let start = data.range(of: Data("<?xml".utf8)),
              let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound..<data.endIndex) else {
            return nil
        }
        let plistData = data.subdata(in: start.lowerBound..<end.upperBound)
        guard let plist = try? PropertyListSerialization.propertyList(from: plistData, format: nil) as? [String: Any]
        else { return nil }
        return plist["ExpirationDate"] as? Date
    }

    static func iso8601(_ date: Date?) -> Any {
        guard let date else { return NSNull() }
        return ISO8601DateFormatter().string(from: date)
    }
}
