import XCTest
import AVFoundation
import CoreVideo
@testable import GCamCameraApp

final class RAWPackTests: XCTestCase {
    func testPortableMagicIsStable() {
        XCTAssertEqual(GCamPortableSettings.rawpackMagic, "GCAMRAW1")
    }

    func testBayerRequestMeetsAVFoundationRequirements() {
        let first = RAWBurstCapture.rawSettings(pixelFormat: kCVPixelFormatType_14Bayer_RGGB)
        let second = RAWBurstCapture.rawSettings(pixelFormat: kCVPixelFormatType_14Bayer_RGGB)
        // AVFoundation throws NSInvalidArgumentException for Bayer RAW unless
        // quality prioritization is speed, even on a quality-capable output.
        XCTAssertEqual(first.photoQualityPrioritization, .speed)
        XCTAssertEqual(first.flashMode, .off)
        XCTAssertEqual(first.rawPhotoPixelFormatType, kCVPixelFormatType_14Bayer_RGGB)
        XCTAssertNil(first.format)
        XCTAssertNotEqual(first.uniqueID, second.uniqueID)
    }

    func testPreviewSamplingPreservesEveryBayerPhase() throws {
        for dimensions in [(4032, 3024), (8064, 6048), (6000, 4000), (4033, 3025)] {
            let sampling = try BayerSampling(width: dimensions.0, height: dimensions.1, maxDimension: 2048)
            XCTAssertLessThanOrEqual(max(sampling.width, sampling.height), 2048)
            XCTAssertEqual(sampling.width % 2, 0)
            XCTAssertEqual(sampling.height % 2, 0)
            for x in 0..<sampling.width {
                XCTAssertEqual(sampling.sourceCoordinate(x) % 2, x % 2)
                XCTAssertLessThan(sampling.sourceCoordinate(x), dimensions.0)
            }
            for y in 0..<sampling.height {
                XCTAssertEqual(sampling.sourceCoordinate(y) % 2, y % 2)
                XCTAssertLessThan(sampling.sourceCoordinate(y), dimensions.1)
            }
        }
    }

    func testFullResolutionSamplingDoesNotDiscardPixels() throws {
        let sampling = try BayerSampling(width: 7, height: 5, maxDimension: nil)
        XCTAssertEqual(sampling.width, 7)
        XCTAssertEqual(sampling.height, 5)
        XCTAssertEqual((0..<7).map(sampling.sourceCoordinate), Array(0..<7))
    }

    func testSamplingRejectsInvalidDimensions() {
        XCTAssertThrowsError(try BayerSampling(width: 0, height: 4, maxDimension: 2048))
        XCTAssertThrowsError(try BayerSampling(width: 4, height: 4, maxDimension: 1))
    }

    func testRAWReaderUsesPaddedRowsAndPreservesMosaic() throws {
        let buffer = try makeBuffer(width: 8, height: 8, format: kCVPixelFormatType_14Bayer_RGGB)
        let full = try RAWBufferReader.read(pixelBuffer: buffer, metadata: [:], frameIndex: 3)
        XCTAssertEqual(full.metadata.rowStrideBytes, 16)
        XCTAssertEqual(full.metadata.frameIndex, 3)
        XCTAssertEqual(full.pixels, (0..<8).flatMap { y in (0..<8).map { x in UInt16(y * 100 + x) } })

        let preview = try RAWBufferReader.read(pixelBuffer: buffer, metadata: [:], frameIndex: 3, maxDimension: 4)
        XCTAssertEqual(preview.metadata.width, 4)
        XCTAssertEqual(preview.metadata.height, 4)
        // Keep all four sensels from each selected cell, not only even/even.
        XCTAssertEqual(preview.pixels, [0, 1, 4, 5, 100, 101, 104, 105,
                                       400, 401, 404, 405, 500, 501, 504, 505])
    }

    func testRAWReaderInfersCFAFromAllSupportedPixelFormats() throws {
        let formats: [(OSType, UInt8)] = [
            (kCVPixelFormatType_14Bayer_RGGB, 0), (kCVPixelFormatType_14Bayer_BGGR, 1),
            (kCVPixelFormatType_14Bayer_GRBG, 2), (kCVPixelFormatType_14Bayer_GBRG, 3)
        ]
        for (format, pattern) in formats {
            let buffer = try makeBuffer(width: 8, height: 8, format: format)
            let frame = try RAWBufferReader.read(pixelBuffer: buffer, metadata: [:], frameIndex: 0)
            XCTAssertEqual(frame.metadata.bayerPattern, pattern)
            XCTAssertEqual(frame.metadata.bitDepth, 14)
            XCTAssertEqual(frame.metadata.whiteLevel, 16383)
        }
    }

    func testInvalidWhiteLevelsReturnErrorsInsteadOfTrapping() throws {
        let buffer = try makeBuffer(width: 8, height: 8, format: kCVPixelFormatType_14Bayer_RGGB)
        for white in [Float.nan, Float.infinity, -1, 0, 65536] {
            let metadata: [String: Any] = ["{DNG}": ["WhiteLevel": NSNumber(value: white)]]
            XCTAssertThrowsError(try RAWBufferReader.read(pixelBuffer: buffer, metadata: metadata, frameIndex: 0))
        }
    }

    func testReaderRejectsNonBayerPixelBuffers() throws {
        let buffer = try makeBuffer(width: 8, height: 8, format: kCVPixelFormatType_32BGRA)
        XCTAssertThrowsError(try RAWBufferReader.read(pixelBuffer: buffer, metadata: [:], frameIndex: 0))
    }

    private func makeBuffer(width: Int, height: Int, format: OSType) throws -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let attributes = [kCVPixelBufferBytesPerRowAlignmentKey: 64] as CFDictionary
        let status = CVPixelBufferCreate(kCFAllocatorDefault, width, height, format, attributes, &buffer)
        XCTAssertEqual(status, kCVReturnSuccess)
        let pixels = try XCTUnwrap(buffer)
        let lockStatus = CVPixelBufferLockBaseAddress(pixels, [])
        XCTAssertEqual(lockStatus, kCVReturnSuccess)
        guard lockStatus == kCVReturnSuccess else {
            throw NSError(domain: "RAWPackTests", code: Int(lockStatus))
        }
        defer { CVPixelBufferUnlockBaseAddress(pixels, []) }
        let base = try XCTUnwrap(CVPixelBufferGetBaseAddress(pixels)).assumingMemoryBound(to: UInt16.self)
        let stride = CVPixelBufferGetBytesPerRow(pixels) / MemoryLayout<UInt16>.stride
        for y in 0..<height {
            for x in 0..<stride {
                base[y * stride + x] = x < width ? UInt16(y * 100 + x) : UInt16.max
            }
        }
        return pixels
    }
}
