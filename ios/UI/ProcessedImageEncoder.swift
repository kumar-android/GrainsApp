import CoreGraphics
import UIKit

enum ProcessedImageEncoder {
    static func jpeg(width: Int, height: Int, linearRGB: [Float]) throws -> Data {
        guard width > 0, height > 0, linearRGB.count >= width * height * 3 else {
            throw NSError(domain: "GCamCamera", code: 50, userInfo: [NSLocalizedDescriptionKey: "Processed RGB buffer has invalid dimensions"])
        }
        var rgba = [UInt8](repeating: 255, count: width * height * 4)
        for index in 0..<(width * height) {
            for channel in 0..<3 {
                let value = max(0, min(1, linearRGB[index * 3 + channel]))
                let encoded = value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1.0 / 2.4) - 0.055
                rgba[index * 4 + channel] = UInt8((max(0, min(1, encoded)) * 255).rounded())
            }
        }
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let image = rgba.withUnsafeMutableBytes({ bytes -> CGImage? in
            guard let context = CGContext(data: bytes.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: colorSpace, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
            return context.makeImage()
        }) else {
            throw NSError(domain: "GCamCamera", code: 51, userInfo: [NSLocalizedDescriptionKey: "Unable to create display image"])
        }
        guard let data = UIImage(cgImage: image).jpegData(compressionQuality: 0.96) else {
            throw NSError(domain: "GCamCamera", code: 52, userInfo: [NSLocalizedDescriptionKey: "Unable to encode JPEG result"])
        }
        return data
    }
}
