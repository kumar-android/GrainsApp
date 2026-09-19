import Foundation

final class GCamProcessor {
    private let engine: OpaquePointer

    init() throws {
        guard let engine = gcam_create_engine() else { throw NSError(domain: "GCamCamera", code: 20, userInfo: [NSLocalizedDescriptionKey: "Unable to allocate processing engine"]) }
        self.engine = engine
    }

    deinit { gcam_destroy_engine(engine) }

    func process(frames: [CapturedRAWFrame], profilePath: String) throws -> (width: Int, height: Int, rgb: [Float], diagnostics: String) {
        var error = [CChar](repeating: 0, count: 1024)
        let loaded = profilePath.withCString { gcam_load_profile(engine, $0, &error, UInt32(error.count)) }
        guard loaded != 0 else { throw NSError(domain: "GCamCamera", code: 21, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        guard gcam_begin_burst(engine, &error, UInt32(error.count)) != 0 else { throw NSError(domain: "GCamCamera", code: 22, userInfo: [NSLocalizedDescriptionKey: String(cString: error)]) }
        for (index, frame) in frames.enumerated() {
            var wb = [frame.metadata.whiteBalance.0, frame.metadata.whiteBalance.1, frame.metadata.whiteBalance.2]
            let added = frame.metadata.lensIdentifier.withCString { lens in
                frame.metadata.sensorIdentifier.withCString { sensor in
                    frame.pixels.withUnsafeBufferPointer { pixels in
                        var view = GcamRawFrameView(width: frame.metadata.width, height: frame.metadata.height, row_stride_bytes: frame.metadata.rowStrideBytes, bit_depth: frame.metadata.bitDepth, bayer_pattern: frame.metadata.bayerPattern, black_level: frame.metadata.blackLevel, white_level: frame.metadata.whiteLevel, iso: frame.metadata.iso, exposure_time_seconds: frame.metadata.exposureTimeSeconds, aperture: frame.metadata.aperture, color_temperature_kelvin: frame.metadata.colorTemperatureKelvin, white_balance: (wb[0], wb[1], wb[2]), orientation: frame.metadata.orientation, timestamp_unix_micros: frame.metadata.timestampUnixMicros, frame_index: UInt32(index), lens_identifier: lens, sensor_identifier: sensor, optional_metadata: nil, pixels: pixels.baseAddress, pixel_count: UInt32(frame.pixels.count))
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
        return (Int(result.width), Int(result.height), rgb, String(cString: diagnostics))
    }
}
