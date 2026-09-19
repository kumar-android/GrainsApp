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
    private var isConfigured = false
    private var continuation: CheckedContinuation<[CapturedRAWFrame], Error>?
    private var targetCount = 8
    private var nextIndex: UInt32 = 0
    private var frames: [CapturedRAWFrame] = []
    private let maximumBurstFrames = 8
    private let minimumBurstFrames = 2
    private let burstMemoryBudget = UInt64(384 * 1024 * 1024)

    func configure() throws {
        try queue.sync {
            guard !isConfigured else { return }
            let camera = try discovery.mainWideCamera()
            session.beginConfiguration()
            defer { session.commitConfiguration() }
            session.sessionPreset = .photo
            guard let input = try? AVCaptureDeviceInput(device: camera), session.canAddInput(input), session.canAddOutput(output) else {
                throw NSError(domain: "GCamCamera", code: 3, userInfo: [NSLocalizedDescriptionKey: "Unable to configure the main camera"])
            }
            session.addInput(input)
            session.addOutput(output)
            do {
                rawType = try discovery.highestQualityRAWType(from: output)
            } catch {
                session.removeOutput(output)
                session.removeInput(input)
                throw error
            }
            if #available(iOS 13.0, *) { output.maxPhotoQualityPrioritization = .quality }
            isConfigured = true
        }
    }

    func start() {
        queue.async {
            guard self.isConfigured, !self.session.isRunning else { return }
            self.session.startRunning()
        }
    }

    func stop() {
        queue.async {
            if self.session.isRunning { self.session.stopRunning() }
            if self.continuation != nil {
                self.finish(.failure(NSError(domain: "GCamCamera", code: 7, userInfo: [NSLocalizedDescriptionKey: "Camera capture was stopped"])))
            }
        }
    }

    @MainActor
    func captureBurst(count: Int) async throws -> [CapturedRAWFrame] {
        return try await withCheckedThrowingContinuation { continuation in
            queue.async {
                guard self.isConfigured, self.rawType != 0 else {
                    continuation.resume(throwing: NSError(domain: "GCamCamera", code: 4, userInfo: [NSLocalizedDescriptionKey: "RAW capture is not configured"]))
                    return
                }
                guard self.session.isRunning else {
                    continuation.resume(throwing: NSError(domain: "GCamCamera", code: 10, userInfo: [NSLocalizedDescriptionKey: "The camera preview is not running"]))
                    return
                }
                guard self.continuation == nil else {
                    continuation.resume(throwing: NSError(domain: "GCamCamera", code: 8, userInfo: [NSLocalizedDescriptionKey: "A RAW burst is already in progress"]))
                    return
                }
                self.targetCount = max(self.minimumBurstFrames, min(count, self.maximumBurstFrames))
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

    private func finish(_ result: Result<[CapturedRAWFrame], Error>) {
        guard let continuation else { return }
        self.continuation = nil
        switch result {
        case .success:
            let capturedFrames = self.frames
            self.frames.removeAll(keepingCapacity: false)
            continuation.resume(returning: capturedFrames)
        case .failure(let error):
            self.frames.removeAll(keepingCapacity: false)
            continuation.resume(throwing: error)
        }
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) {
        queue.async {
            if let error {
                self.finish(.failure(error))
                return
            }
            do {
                guard let pixelBuffer = photo.pixelBuffer else { throw NSError(domain: "GCamCamera", code: 5, userInfo: [NSLocalizedDescriptionKey: "RAW pixel buffer missing"]) }
                self.frames.append(try RAWBufferReader.read(pixelBuffer: pixelBuffer, metadata: photo.metadata, frameIndex: self.nextIndex))
                self.nextIndex += 1
                if self.frames.count == 1 {
                    let bytesPerFrame = UInt64(self.frames[0].pixels.count) * UInt64(MemoryLayout<UInt16>.size)
                    let deviceBudget = min(self.burstMemoryBudget, ProcessInfo.processInfo.physicalMemory / 8)
                    let safeCount = bytesPerFrame == 0 ? self.minimumBurstFrames : Int(deviceBudget / bytesPerFrame)
                    self.targetCount = min(self.targetCount, max(self.minimumBurstFrames, min(self.maximumBurstFrames, safeCount)))
                }
                if self.frames.count < self.targetCount { self.captureNext() }
                else {
                    self.finish(.success(self.frames))
                }
            } catch {
                self.finish(.failure(error))
            }
        }
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishCaptureFor resolvedSettings: AVCaptureResolvedPhotoSettings, error: Error?) {
        guard let error else { return }
        queue.async { self.finish(.failure(error)) }
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
        guard width > 0, height > 0, stride >= width * MemoryLayout<UInt16>.stride, stride % MemoryLayout<UInt16>.stride == 0,
              CVPixelBufferGetDataSize(pixelBuffer) >= stride * height else {
            throw NSError(domain: "GCamCamera", code: 9, userInfo: [NSLocalizedDescriptionKey: "RAW pixel buffer has an unsupported layout"])
        }
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
