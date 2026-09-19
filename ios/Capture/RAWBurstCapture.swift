@preconcurrency import AVFoundation
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

// All mutable capture state is confined to queue. The session is exposed only
// for AVCaptureVideoPreviewLayer; callers must not configure it.
final class RAWBurstCapture: NSObject, AVCapturePhotoCaptureDelegate, @unchecked Sendable {
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
    private var processingMaxDimension: Int?
    private var activeRequestID: Int64?
    private var receivedRAW = false
    private let maximumBurstFrames = 8
    private let minimumBurstFrames = 2
    private let burstMemoryBudget = UInt64(384 * 1024 * 1024)

    private func configureSession() throws {
        guard !isConfigured else { return }
        let camera = try discovery.mainWideCamera()
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        session.sessionPreset = .photo
        let input = try AVCaptureDeviceInput(device: camera)
        guard session.canAddInput(input), session.canAddOutput(output) else {
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
        output.maxPhotoQualityPrioritization = .speed
        isConfigured = true
    }

    func start() async throws {
        try Task.checkCancellation()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                do {
                    try self.configureSession()
                    if !self.session.isRunning { self.session.startRunning() }
                    guard self.session.isRunning else {
                        throw NSError(domain: "GCamCamera", code: 10, userInfo: [NSLocalizedDescriptionKey: "The camera preview could not be started"])
                    }
                    continuation.resume()
                } catch {
                    continuation.resume(throwing: error)
                }
            }
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
    func captureBurst(count: Int, maxDimension: Int? = nil) async throws -> [CapturedRAWFrame] {
        try Task.checkCancellation()
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
                self.processingMaxDimension = maxDimension
                self.frames.removeAll(keepingCapacity: true)
                self.continuation = continuation
                self.captureNext()
            }
        }
    }

    private func captureNext() {
        guard continuation != nil else { return }
        guard session.isRunning, !session.isInterrupted,
              let connection = output.connection(with: .video), connection.isEnabled, connection.isActive else {
            finish(.failure(NSError(domain: "GCamCamera", code: 10, userInfo: [NSLocalizedDescriptionKey: "The camera is not available for capture"])))
            return
        }
        guard output.availableRawPhotoPixelFormatTypes.contains(rawType) else {
            finish(.failure(NSError(domain: "GCamCamera", code: 4, userInfo: [NSLocalizedDescriptionKey: "The selected RAW format is no longer available"])))
            return
        }
        let settings = Self.rawSettings(pixelFormat: rawType)
        activeRequestID = settings.uniqueID
        receivedRAW = false
        output.capturePhoto(with: settings, delegate: self)
    }

    static func rawSettings(pixelFormat: OSType) -> AVCapturePhotoSettings {
        let settings = AVCapturePhotoSettings(rawPixelFormatType: pixelFormat)
        settings.flashMode = .off
        // Bayer RAW requires .speed. .quality raises NSInvalidArgumentException
        // in capturePhoto, which cannot be caught by a Swift do/catch.
        settings.photoQualityPrioritization = .speed
        return settings
    }

    private func finish(_ result: Result<[CapturedRAWFrame], Error>) {
        guard let continuation else { return }
        self.continuation = nil
        activeRequestID = nil
        receivedRAW = false
        switch result {
        case .success:
            let capturedFrames = self.frames
            self.frames.removeAll(keepingCapacity: false)
            self.processingMaxDimension = nil
            continuation.resume(returning: capturedFrames)
        case .failure(let error):
            self.frames.removeAll(keepingCapacity: false)
            self.processingMaxDimension = nil
            continuation.resume(throwing: error)
        }
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) {
        queue.async {
            guard self.continuation != nil, self.activeRequestID == photo.resolvedSettings.uniqueID else { return }
            if let error {
                self.finish(.failure(error))
                return
            }
            guard photo.isRawPhoto, !self.receivedRAW else { return }
            do {
                guard let pixelBuffer = photo.pixelBuffer else {
                    throw NSError(domain: "GCamCamera", code: 5, userInfo: [NSLocalizedDescriptionKey: "RAW pixel buffer missing"])
                }
                let frame = try RAWBufferReader.read(pixelBuffer: pixelBuffer, metadata: photo.metadata, frameIndex: self.nextIndex, maxDimension: self.processingMaxDimension)
                self.frames.append(frame)
                self.receivedRAW = true
                self.nextIndex += 1
                if self.frames.count == 1 {
                    let bytesPerFrame = UInt64(frame.pixels.count) * UInt64(MemoryLayout<UInt16>.size)
                    let deviceBudget = min(self.burstMemoryBudget, ProcessInfo.processInfo.physicalMemory / 8)
                    let safeCount = bytesPerFrame == 0 ? 0 : Int(deviceBudget / bytesPerFrame)
                    guard safeCount >= self.minimumBurstFrames else {
                        throw NSError(domain: "GCamCamera", code: 11, userInfo: [NSLocalizedDescriptionKey: "This RAW resolution exceeds the safe burst memory budget"])
                    }
                    self.targetCount = min(self.targetCount, min(self.maximumBurstFrames, safeCount))
                }
            } catch {
                self.finish(.failure(error))
            }
        }
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishCaptureFor resolvedSettings: AVCaptureResolvedPhotoSettings, error: Error?) {
        let requestID = resolvedSettings.uniqueID
        queue.async {
            guard self.continuation != nil, self.activeRequestID == requestID else { return }
            if let error {
                self.finish(.failure(error))
                return
            }
            guard self.receivedRAW else {
                self.finish(.failure(NSError(domain: "GCamCamera", code: 5, userInfo: [NSLocalizedDescriptionKey: "Capture completed without a RAW photo"])))
                return
            }
            // Only this final callback ends a capture request. Late callbacks
            // from stopped/failed requests must not affect a subsequent burst.
            self.activeRequestID = nil
            if self.frames.count < self.targetCount { self.captureNext() }
            else { self.finish(.success(self.frames)) }
        }
    }
}

enum RAWBufferReader {
    static func read(pixelBuffer: CVPixelBuffer, metadata: [String: Any], frameIndex: UInt32, maxDimension: Int? = nil) throws -> CapturedRAWFrame {
        guard !CVPixelBufferIsPlanar(pixelBuffer),
              CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly) == kCVReturnSuccess else {
            throw NSError(domain: "GCamCamera", code: 6, userInfo: [NSLocalizedDescriptionKey: "Unable to access the RAW pixel buffer"])
        }
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
        let pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer)
        guard let formatBayer = bayerCode(pixelFormat: pixelFormat) else {
            throw NSError(domain: "GCamCamera", code: 9, userInfo: [NSLocalizedDescriptionKey: "Unsupported RAW pixel format: \(pixelFormat)"])
        }
        let sampling = try BayerSampling(width: width, height: height, maxDimension: maxDimension)
        let outputWidth = sampling.width
        let outputHeight = sampling.height
        var pixels = [UInt16](repeating: 0, count: outputWidth * outputHeight)
        for y in 0..<outputHeight {
            let sourceY = sampling.sourceCoordinate(y)
            for x in 0..<outputWidth {
                let sourceX = sampling.sourceCoordinate(x)
                pixels[y * outputWidth + x] = source[sourceY * samplesPerRow + sourceX]
            }
        }

        let dng = metadata["{DNG}"] as? [String: Any] ?? metadata
        func firstFloat(_ value: Any?) -> Float? {
            if let number = value as? NSNumber { return number.floatValue }
            if let numbers = value as? [NSNumber] { return numbers.first?.floatValue }
            return nil
        }
        let black = firstFloat(dng["BlackLevel"]) ?? 0
        let white = firstFloat(dng["WhiteLevel"]) ?? Float((1 << 14) - 1)
        guard black.isFinite, white.isFinite, black >= 0, white > black, white <= Float(UInt16.max) else {
            throw NSError(domain: "GCamCamera", code: 12, userInfo: [NSLocalizedDescriptionKey: "RAW black/white levels are invalid"])
        }
        let iso = (metadata["{Exif}"] as? [String: Any])?["ISOSpeedRatings"] as? [NSNumber]
        let exposure = ((metadata["{Exif}"] as? [String: Any])?["ExposureTime"] as? NSNumber)?.floatValue ?? 0
        let aperture = ((metadata["{Exif}"] as? [String: Any])?["FNumber"] as? NSNumber)?.floatValue ?? 0
        // The pixel format defines the delivered buffer's CFA order even when
        // AVFoundation omits DNG CFAPattern from photo.metadata.
        let bayer = formatBayer
        let wb: (Float, Float, Float) = (dng["AsShotNeutral"] as? [NSNumber]).map { values in
            let neutral = values.map(\.floatValue)
            return (1 / max(neutral[safe: 0] ?? 1, 0.01), 1 / max(neutral[safe: 1] ?? 1, 0.01), 1 / max(neutral[safe: 2] ?? 1, 0.01))
        } ?? (1.0, 1.0, 1.0)
        let whiteLevelEstimate = max(2, Int(white) + 1)
        let bitDepthEstimate = Int(ceil(log2(Double(whiteLevelEstimate))))
        let detectedBitDepth = UInt16(max(1, min(16, bitDepthEstimate)))
        let rawMetadata = GcamRawMetadata(width: UInt32(outputWidth), height: UInt32(outputHeight), rowStrideBytes: UInt32(outputWidth * MemoryLayout<UInt16>.size), bitDepth: detectedBitDepth, bayerPattern: UInt8(bayer), blackLevel: black, whiteLevel: white, iso: Float(iso?.first?.floatValue ?? 100), exposureTimeSeconds: exposure, aperture: aperture, colorTemperatureKelvin: 0, whiteBalance: wb, orientation: 1, timestampUnixMicros: Int64(Date().timeIntervalSince1970 * 1_000_000), frameIndex: frameIndex, lensIdentifier: "AVFoundation main wide", sensorIdentifier: "runtime DNG metadata")
        return CapturedRAWFrame(metadata: rawMetadata, pixels: pixels)
    }
}

private func bayerCode(pixelFormat: OSType) -> UInt8? {
    switch pixelFormat {
    case kCVPixelFormatType_14Bayer_RGGB: return 0
    case kCVPixelFormatType_14Bayer_BGGR: return 1
    case kCVPixelFormatType_14Bayer_GRBG: return 2
    case kCVPixelFormatType_14Bayer_GBRG: return 3
    default: return nil
    }
}

// Sample complete 2x2 Bayer cells. Taking x * scale with an even scale
// selects just one CFA phase and loses the other color channels.
struct BayerSampling {
    let scale: Int
    let width: Int
    let height: Int

    init(width: Int, height: Int, maxDimension: Int?) throws {
        guard width >= 2, height >= 2, maxDimension.map({ $0 >= 2 }) ?? true else {
            throw NSError(domain: "GCamCamera", code: 9, userInfo: [NSLocalizedDescriptionKey: "RAW sampling dimensions are invalid"])
        }
        if let maxDimension, max(width, height) > maxDimension {
            let longestCellCount = max(width / 2, height / 2)
            let targetCellCount = maxDimension / 2
            scale = 1 + (longestCellCount - 1) / targetCellCount
            self.width = 2 * (1 + (width / 2 - 1) / scale)
            self.height = 2 * (1 + (height / 2 - 1) / scale)
        } else {
            scale = 1
            self.width = width
            self.height = height
        }
    }

    func sourceCoordinate(_ coordinate: Int) -> Int {
        (coordinate / 2) * (2 * scale) + coordinate % 2
    }
}

private extension Array {
    subscript(safe index: Index) -> Element? { indices.contains(index) ? self[index] : nil }
}
