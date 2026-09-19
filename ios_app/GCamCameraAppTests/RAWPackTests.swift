import XCTest
@testable import GCamCameraApp

final class RAWPackTests: XCTestCase {
    func testPortableMagicIsStable() {
        XCTAssertEqual(GCamPortableSettings.rawpackMagic, "GCAMRAW1")
    }
}
