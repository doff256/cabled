@testable import Companion
import XCTest

final class SigningInfoTests: XCTestCase {
    func testReadsExpiryFromProvisionEnvelope() throws {
        let plist = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0"><dict>
          <key>Name</key><string>iOS Team Provisioning Profile</string>
          <key>ExpirationDate</key><date>2026-11-28T10:00:00Z</date>
        </dict></plist>
        """
        // Real files wrap the plist in binary CMS data; imitate that with junk on both sides.
        var data = Data([0x30, 0x82, 0x01, 0xFF, 0x00, 0x13])
        data.append(Data(plist.utf8))
        data.append(Data([0x00, 0xA0, 0x82, 0x0B]))

        let date = try XCTUnwrap(SigningInfo.expirationDate(fromProvision: data))
        XCTAssertEqual(ISO8601DateFormatter().string(from: date), "2026-11-28T10:00:00Z")
    }

    func testMissingOrBrokenProvision() {
        XCTAssertNil(SigningInfo.expirationDate(fromProvision: Data()))
        XCTAssertNil(SigningInfo.expirationDate(fromProvision: Data("<?xml no end".utf8)))
    }

    func testNoProvisionInTestBundle() {
        // Simulator builds carry no embedded.mobileprovision.
        XCTAssertNil(SigningInfo.expirationDate(bundle: Bundle(for: Self.self)))
    }
}
