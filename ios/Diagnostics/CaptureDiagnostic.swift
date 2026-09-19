import Foundation

struct CaptureDiagnostic: Codable, Sendable {
    let camera: String
    let device: String
    let lens: String
    let rawFormat: String
    let dimensions: String
    let bitDepth: Int
    let bayer: String
    let iso: Float
    let shutter: Float
    let frames: Int
    let burstDurationSeconds: Double
}
