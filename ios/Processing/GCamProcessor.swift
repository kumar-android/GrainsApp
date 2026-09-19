import Foundation

final class GCamProcessor {
    private static let maxOnDeviceDimension = 2048
    private static let maxEstimatedWorkingSet = UInt64(1_200_000_000)
    private let engine: OpaquePointer

    init() throws {
        guard let engine = gcam_create_engine() else { throw NSError(domain: "GCamCamera", code: 20, userInfo: [NSLocalizedDescriptionKey: "Unable to allocate processing engine"]) }
        self.engine = engine
    }

    deinit { gcam_destroy_engine(engine) }

    func process(frames: [CapturedRAWFrame], profilePath: String) throws -> (width: Int, height: Int, rgb: [Float], diagnostics: String) {
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
        let previewNote = processingScale > 1 ? "\n\nOn-device preview scale: 1/\(processingScale). The exported RAW burst remains full resolution." : ""
        return (Int(result.width), Int(result.height), rgb, diagnosticText + previewNote)
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
        let sampling = try BayerSampling(width: Int(first.metadata.width), height: Int(first.metadata.height), maxDimension: maxOnDeviceDimension)
        let scale = sampling.scale

        let originalPixels = UInt64(first.metadata.width) * UInt64(first.metadata.height)
        let sourceBytes = originalPixels * UInt64(frames.count) * UInt64(MemoryLayout<UInt16>.size)
        let workingWidth = sampling.width
        let workingHeight = sampling.height
        let workingPixels = UInt64(workingWidth) * UInt64(workingHeight)
        // Account for Swift's source RAW, the C++ RAW copy, the two normalized
        // planes, the merged plane, and the RGB/intermediate render buffers.
        let estimatedBytes = sourceBytes + workingPixels * UInt64(frames.count) * 20 + workingPixels * 16
        let deviceBudget = min(Self.maxEstimatedWorkingSet, ProcessInfo.processInfo.physicalMemory / 2)
        guard estimatedBytes <= deviceBudget else {
            throw NSError(domain: "GCamCamera", code: 29, userInfo: [NSLocalizedDescriptionKey: "This burst is too large for safe on-device processing. Export the full RAW burst and process it with the Windows RAWPACK laboratory."])
        }

        guard sampling.width != Int(first.metadata.width) || sampling.height != Int(first.metadata.height) else { return (frames, 1) }
        let reduced = frames.map { frame -> CapturedRAWFrame in
            let width = Int(frame.metadata.width)
            let outputWidth = sampling.width
            let outputHeight = sampling.height
            var pixels = [UInt16](repeating: 0, count: outputWidth * outputHeight)
            for y in 0..<outputHeight {
                let sourceY = sampling.sourceCoordinate(y)
                for x in 0..<outputWidth {
                    let sourceX = sampling.sourceCoordinate(x)
                    pixels[y * outputWidth + x] = frame.pixels[sourceY * width + sourceX]
                }
            }
            var metadata = frame.metadata
            metadata.width = UInt32(outputWidth)
            metadata.height = UInt32(outputHeight)
            metadata.rowStrideBytes = UInt32(outputWidth * MemoryLayout<UInt16>.size)
            return CapturedRAWFrame(metadata: metadata, pixels: pixels)
        }
        return (reduced, scale)
    }
}
