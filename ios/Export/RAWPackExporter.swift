import Foundation

enum RAWPackExporter {
    static func write(frames: [CapturedRAWFrame], to directory: URL) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var manifest = "# GCAMRAW1 burst manifest\n"
        for frame in frames {
            let path = directory.appendingPathComponent(String(format: "frame_%04u.rawpack", frame.metadata.frameIndex))
            var data = Data("GCAMRAW1".utf8)
            data.appendLE(UInt32(1))
            data.appendLE(frame.metadata.width)
            data.appendLE(frame.metadata.height)
            data.appendLE(frame.metadata.rowStrideBytes)
            data.appendLE(frame.metadata.bitDepth)
            data.append(UInt8(frame.metadata.bayerPattern)); data.append(0); data.appendLE(UInt16(0))
            data.appendLE(frame.metadata.blackLevel); data.appendLE(frame.metadata.whiteLevel); data.appendLE(frame.metadata.iso)
            data.appendLE(frame.metadata.exposureTimeSeconds); data.appendLE(frame.metadata.aperture); data.appendLE(frame.metadata.colorTemperatureKelvin)
            data.appendLE(frame.metadata.whiteBalance.0); data.appendLE(frame.metadata.whiteBalance.1); data.appendLE(frame.metadata.whiteBalance.2)
            data.appendLE(frame.metadata.orientation); data.appendLE(frame.metadata.timestampUnixMicros); data.appendLE(frame.metadata.frameIndex)
            data.appendString(frame.metadata.lensIdentifier); data.appendString(frame.metadata.sensorIdentifier); data.appendString("iOS AVFoundation RAW")
            frame.pixels.forEach { data.appendLE($0) }
            try data.write(to: path, options: .atomic)
            manifest += path.lastPathComponent + "\n"
        }
        try manifest.data(using: .utf8)?.write(to: directory.appendingPathComponent("burst.burst"), options: .atomic)
    }
}

private extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { append(contentsOf: $0) }
    }
    mutating func appendLE(_ value: Float) {
        appendLE(value.bitPattern)
    }
    mutating func appendString(_ value: String) {
        let bytes = Data(value.utf8); appendLE(UInt32(bytes.count)); append(bytes)
    }
}
