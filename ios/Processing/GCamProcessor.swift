import Foundation

final class GCamProcessor {
    // The on-device path runs the vector reference engine, so it bounds a burst by
    // how many frames fit the memory budget rather than by resolution. Reducing
    // every frame on the same grid while capturing removes the sub-pixel phase
    // diversity that alignment and the merge depend on, which collapses the result
    // back into a single decimated RAW frame.
    private static let maximumWorkingSet = UInt64(1_200_000_000)
    private static let minimumProcessedFrames = 2
    // Peak bytes per working pixel per frame: the engine packs each RAW into one
    // normalized 16-bit plane (2). The captured frames are counted separately by
    // `sourceBytes`. The merged plane, the alignment luma scratch, and the render
    // copies the denoise and detail stages hold add a frame-independent 32.
    private static let bytesPerWorkingPixelPerFrame = UInt64(2)
    private static let fixedBytesPerWorkingPixel = UInt64(32)
    private let engine: OpaquePointer

    init() throws {
        guard let engine = gcam_create_engine() else { throw NSError(domain: "GCamCamera", code: 20, userInfo: [NSLocalizedDescriptionKey: "Unable to allocate processing engine"]) }
        self.engine = engine
    }

    deinit { gcam_destroy_engine(engine) }

    func process(frames: [CapturedRAWFrame], profilePath: String) throws -> (width: Int, height: Int, rgb: [Float], diagnostics: String, summary: String) {
        guard !frames.isEmpty else {
            throw NSError(domain: "GCamCamera", code: 27, userInfo: [NSLocalizedDescriptionKey: "Cannot process an empty RAW burst"])
        }
        let prepared = try Self.prepareForOnDeviceProcessing(frames)
        let workingFrames = prepared.frames
        let processingScale = prepared.scale
        var error = [CChar](repeating: 0, count: 1024)
        let loaded = profilePath.withCString { gcam_load_profile(engine, $0, &error, UInt32(error.count)) }
        guard loaded != 0 else { throw NSError(domain: "GCamCamera", code: 21, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        guard gcam_begin_burst(engine, &error, UInt32(error.count)) != 0 else { throw NSError(domain: "GCamCamera", code: 22, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        for frame in workingFrames {
            let wb = [frame.metadata.whiteBalance.0, frame.metadata.whiteBalance.1, frame.metadata.whiteBalance.2]
            let added = frame.metadata.lensIdentifier.withCString { lens in
                frame.metadata.sensorIdentifier.withCString { sensor in
                    frame.pixels.withUnsafeBufferPointer { pixels in
                        var view = GcamRawFrameView(width: frame.metadata.width, height: frame.metadata.height, row_stride_bytes: frame.metadata.rowStrideBytes, bit_depth: frame.metadata.bitDepth, bayer_pattern: frame.metadata.bayerPattern, black_level: frame.metadata.blackLevel, white_level: frame.metadata.whiteLevel, iso: frame.metadata.iso, exposure_time_seconds: frame.metadata.exposureTimeSeconds, aperture: frame.metadata.aperture, color_temperature_kelvin: frame.metadata.colorTemperatureKelvin, white_balance: (wb[0], wb[1], wb[2]), orientation: frame.metadata.orientation, timestamp_unix_micros: frame.metadata.timestampUnixMicros, frame_index: frame.metadata.frameIndex, lens_identifier: lens, sensor_identifier: sensor, optional_metadata: nil, pixels: pixels.baseAddress, pixel_count: UInt32(frame.pixels.count))
                        return gcam_add_raw_frame(engine, &view, &error, UInt32(error.count))
                    }
                }
            }
            guard added != 0 else { throw NSError(domain: "GCamCamera", code: 23, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        }
        guard gcam_process_burst(engine, &error, UInt32(error.count)) != 0 else { throw NSError(domain: "GCamCamera", code: 24, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        var result = GcamImageView(width: 0, height: 0, rgb_linear: nil, float_count: 0)
        guard gcam_get_result(engine, &result, &error, UInt32(error.count)) != 0, let base = result.rgb_linear else { throw NSError(domain: "GCamCamera", code: 25, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        let rgb = Array(UnsafeBufferPointer(start: base, count: Int(result.float_count)))
        var diagnostics = [CChar](repeating: 0, count: 32 * 1024)
        _ = gcam_get_diagnostics_json(engine, &diagnostics, UInt32(diagnostics.count))
        let diagnosticText = String(cString: diagnostics)
        let mergeNote = workingFrames.count < frames.count
            ? "merged \(workingFrames.count) of \(frames.count) frames"
            : "merged \(frames.count) frames"
        let scaleNote = processingScale > 1 ? " · preview scale 1/\(processingScale)" : ""
        let summary = "\(result.width)x\(result.height) · \(mergeNote)\(scaleNote)"
        return (Int(result.width), Int(result.height), rgb, diagnosticText, summary)
    }

    private static func prepareForOnDeviceProcessing(_ frames: [CapturedRAWFrame]) throws -> (frames: [CapturedRAWFrame], scale: Int) {
        guard let first = frames.first else {
            throw NSError(domain: "GCamCamera", code: 27, userInfo: [NSLocalizedDescriptionKey: "Cannot process an empty RAW burst"])
        }
        for frame in frames {
            let width = Int(frame.metadata.width)
            let height = Int(frame.metadata.height)
            let (pixelCount, overflow) = width.multipliedReportingOverflow(by: height)
            guard width >= 2, height >= 2, !overflow,
                  pixelCount <= frame.pixels.count, frame.pixels.count <= Int(UInt32.max),
                  frame.metadata.width == first.metadata.width,
                  frame.metadata.height == first.metadata.height,
                  frame.metadata.bayerPattern == first.metadata.bayerPattern else {
                throw NSError(domain: "GCamCamera", code: 28, userInfo: [NSLocalizedDescriptionKey: "RAW burst dimensions, pixel storage, or Bayer patterns are invalid"])
            }
        }
        let width = Int(first.metadata.width)
        let height = Int(first.metadata.height)
        // Every captured frame stays resident while the engine runs, so the source
        // RAWs are counted once no matter how many frames the engine receives.
        let sourceBytes = UInt64(width) * UInt64(height) * UInt64(frames.count) * UInt64(MemoryLayout<UInt16>.size)
        let deviceBudget = min(Self.maximumWorkingSet, ProcessInfo.processInfo.physicalMemory / 4)
        guard deviceBudget > sourceBytes else {
            throw NSError(domain: "GCamCamera", code: 29, userInfo: [NSLocalizedDescriptionKey: "This burst is too large for safe on-device processing. Export the full RAW burst and process it with the Windows RAWPACK laboratory."])
        }
        let processingBudget = deviceBudget - sourceBytes

        // Full resolution is tried first and frames are dropped before resolution:
        // a shorter full-resolution burst keeps more real detail than a longer one
        // that was reduced on the sampling grid shared by every frame.
        let requiredFrames = min(Self.minimumProcessedFrames, frames.count)
        var scale = 1
        var selection: (sampling: BayerSampling, frameLimit: Int)?
        while selection == nil {
            let sampling = try BayerSampling(width: width, height: height, scale: scale)
            let workingPixels = UInt64(sampling.width) * UInt64(sampling.height)
            let fixedBytes = workingPixels * Self.fixedBytesPerWorkingPixel
            let available = processingBudget > fixedBytes ? processingBudget - fixedBytes : 0
            let bytesPerFrame = workingPixels * Self.bytesPerWorkingPixelPerFrame
            let affordableFrames = bytesPerFrame == 0 ? 0 : Int(available / bytesPerFrame)
            let frameLimit = min(frames.count, affordableFrames)
            if frameLimit >= requiredFrames {
                selection = (sampling, frameLimit)
                break
            }
            // Stop once a larger factor cannot remove any more sensels.
            let next = try BayerSampling(width: width, height: height, scale: scale + 1)
            if next.width == sampling.width && next.height == sampling.height { break }
            scale += 1
        }
        guard let selection else {
            throw NSError(domain: "GCamCamera", code: 29, userInfo: [NSLocalizedDescriptionKey: "This burst is too large for safe on-device processing. Export the full RAW burst and process it with the Windows RAWPACK laboratory."])
        }

        let workingFrames = selection.sampling.scale == 1
            ? frames
            : frames.map { Self.reducedFrame($0, using: selection.sampling) }
        return (Array(workingFrames.prefix(selection.frameLimit)), selection.sampling.scale)
    }

    private static func reducedFrame(_ frame: CapturedRAWFrame, using sampling: BayerSampling) -> CapturedRAWFrame {
        let sourceWidth = Int(frame.metadata.width)
        let outputWidth = sampling.width
        let outputHeight = sampling.height
        var pixels = [UInt16](repeating: 0, count: outputWidth * outputHeight)
        // Box-average each cell the smaller grid replaces instead of picking one sensel out of
        // it, so the detail the reduced grid cannot hold is filtered out rather than folded back
        // in as moire.
        pixels.withUnsafeMutableBufferPointer { destination in
            frame.pixels.withUnsafeBufferPointer { source in
                guard let base = source.baseAddress, let out = destination.baseAddress else { return }
                for y in 0..<outputHeight {
                    for x in 0..<outputWidth {
                        out[y * outputWidth + x] = sampling.reducedSample(base, samplesPerRow: sourceWidth, x: x, y: y)
                    }
                }
            }
        }
        var metadata = frame.metadata
        metadata.width = UInt32(outputWidth)
        metadata.height = UInt32(outputHeight)
        metadata.rowStrideBytes = UInt32(outputWidth * MemoryLayout<UInt16>.size)
        return CapturedRAWFrame(metadata: metadata, pixels: pixels)
    }
}
