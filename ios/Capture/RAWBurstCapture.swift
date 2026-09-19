import AVFoundation
import CoreVideo

struct CapturedRAWFrame: Sendable {
    let metadata: GcamRawMetadata
    let pixels: [UInt16]
}

struct GcamRawMetadata: Sendable {
    var width: UInt32
    var height: UInt32
    var rowStrideBytes: UInt32
    var bitDepth: UInt16
    var bayerPattern: UInt8
    var blackLevel: Float
    var whiteLevel: Float
    var iso: Float
    var exposureTimeSeconds: Float
    var aperture: Float
    var colorTemperatureKelvin: Float
    var whiteBalance: (Float, Float, Float)
    var orientation: Int32
    var timestampUnixMicros: Int64
    var frameIndex: UInt32
    var lensIdentifier: String
    var sensorIdentifier: String
}

final class RAWBurstCapture: NSObject, AVCapturePhotoCaptureDelegate {
    let session = AVCaptureSession()
    private let output = AVCapturePhotoOutput()
    private let queue = DispatchQueue(label: "gcam.capture.serial")
    private let discovery = RAWDeviceDiscovery()
    private var rawType: OSType = 0
    private var continuation: CheckedContinuation<[CapturedRAWFrame], Error>?
    private var targetCount = 8
    private var nextIndex: UInt32 = 0
    private var frames: [CapturedRAWFrame] = []

    func configure() throws {
        let camera = try discovery.mainWideCamera()
        session.beginConfiguration()
        session.sessionPreset = .photo
        guard let input = try? AVCaptureDeviceInput(device: camera), session.canAddInput(input), session.canAddOutput(output) else {
            session.commitConfiguration()
            throw NSError(domain: "GCamCamera", code: 3, userInfo: [NSLocalizedDescriptionKey: "Unable to configure the main camera"])
        }
        session.addInput(input)
        session.addOutput(output)
        rawType = try discovery.highestQualityRAWType(from: output)
        if #available(iOS 13.0, *) { output.maxPhotoQualityPrioritization = .quality }
        session.commitConfiguration()
    }

    func start() { queue.async { self.session.startRunning() } }
    func stop() { queue.async { self.session.stopRunning() } }

    @MainActor
    func captureBurst(count: Int) async throws -> [CapturedRAWFrame] {
        guard rawType != 0 else { throw NSError(domain: "GCamCamera", code: 4, userInfo: [NSLocalizedDescriptionKey: "RAW capture is not configured"]) }
        return try await withCheckedThrowingContinuation { continuation in
            queue.async {
                self.targetCount = max(1, min(count, 12))
                self.nextIndex = 0
                self.frames.removeAll(keepingCapacity: true)
                self.continuation = continuation
                self.captureNext()
            }
        }
    }

    private func captureNext() {
        var settings = AVCapturePhotoSettings(rawPixelFormatType: rawType)
        settings.flashMode = .off
        if #available(iOS 13.0, *) { settings.photoQualityPrioritization = .quality }
        output.capturePhoto(with: settings, delegate: self)
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) {
        queue.async {
            if let error {
                self.continuation?.resume(throwing: error)
                self.continuation = nil
                return
            }
            do {
                guard let pixelBuffer = photo.pixelBuffer else { throw NSError(domain: "GCamCamera", code: 5, userInfo: [NSLocalizedDescriptionKey: "RAW pixel buffer missing"]) }
                self.frames.append(try RAWBufferReader.read(pixelBuffer: pixelBuffer, metadata: photo.metadata, frameIndex: self.nextIndex))
                self.nextIndex += 1
                if self.frames.count < self.targetCount { self.captureNext() }
                else {
                    self.continuation?.resume(returning: self.frames)
                    self.continuation = nil
                }
            } catch {
                self.continuation?.resume(throwing: error)
                self.continuation = nil
            }
        }
    }
}

enum RAWBufferReader {
    static func read(pixelBuffer: CVPixelBuffer, metadata: [String: Any], frameIndex: UInt32) throws -> CapturedRAWFrame {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { throw NSError(domain: "GCamCamera", code: 6, userInfo: [NSLocalizedDescriptionKey: "RAW base address missing"]) }
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let stride = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let source = base.assumingMemoryBound(to: UInt16.self)
        let samplesPerRow = stride / MemoryLayout<UInt16>.stride
        var pixels = [UInt16](repeating: 0, count: width * height)
        for y in 0..<height {
            for x in 0..<width { pixels[y * width + x] = source[y * samplesPerRow + x] }
        }

        let dng = metadata["{DNG}"] as? [String: Any] ?? metadata
        func firstFloat(_ value: Any?) -> Float? {
            if let number = value as? NSNumber { return number.floatValue }
            if let numbers = value as? [NSNumber] { return numbers.first?.floatValue }
            return nil
        }
        let black = firstFloat(dng["BlackLevel"]) ?? 0
        let white = firstFloat(dng["WhiteLevel"]) ?? Float((1 << 16) - 1)
        let iso = (metadata["{Exif}"] as? [String: Any])?["ISOSpeedRatings"] as? [NSNumber]
        let exposure = ((metadata["{Exif}"] as? [String: Any])?["ExposureTime"] as? NSNumber)?.floatValue ?? 0
        let aperture = ((metadata["{Exif}"] as? [String: Any])?["FNumber"] as? NSNumber)?.floatValue ?? 0
        let bayer = bayerCode(from: dng["CFAPattern"] as? [NSNumber])
        let wb: (Float, Float, Float) = (dng["AsShotNeutral"] as? [NSNumber]).map { values in
            let neutral = values.map(\.floatValue)
            return (1 / max(neutral[safe: 0] ?? 1, 0.01), 1 / max(neutral[safe: 1] ?? 1, 0.01), 1 / max(neutral[safe: 2] ?? 1, 0.01))
        } ?? (1.0, 1.0, 1.0)
        let whiteLevelEstimate = max(2, Int(white) + 1)
        let bitDepthEstimate = Int(ceil(log2(Double(whiteLevelEstimate))))
        let detectedBitDepth = UInt16(max(1, min(16, bitDepthEstimate)))
        let rawMetadata = GcamRawMetadata(width: UInt32(width), height: UInt32(height), rowStrideBytes: UInt32(stride), bitDepth: detectedBitDepth, bayerPattern: UInt8(bayer), blackLevel: black, whiteLevel: white, iso: Float(iso?.first?.floatValue ?? 100), exposureTimeSeconds: exposure, aperture: aperture, colorTemperatureKelvin: 0, whiteBalance: wb, orientation: 1, timestampUnixMicros: Int64(Date().timeIntervalSince1970 * 1_000_000), frameIndex: frameIndex, lensIdentifier: "AVFoundation main wide", sensorIdentifier: "runtime DNG metadata")
        return CapturedRAWFrame(metadata: rawMetadata, pixels: pixels)
    }
}

private func bayerCode(from values: [NSNumber]?) -> UInt8 {
    guard let values, values.count >= 4 else { return 255 }
    let pattern = values.prefix(4).map(\.intValue)
    switch pattern {
    case [0, 1, 1, 2]: return 0 // RGGB
    case [2, 1, 1, 0]: return 1 // BGGR
    case [1, 0, 2, 1]: return 2 // GRBG
    case [1, 2, 0, 1]: return 3 // GBRG
    default: return 255
    }
}

private extension Array {
    subscript(safe index: Index) -> Element? { indices.contains(index) ? self[index] : nil }
}
