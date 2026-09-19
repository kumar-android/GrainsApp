import Foundation

enum CaptureState: String, Codable, Sendable {
    case idle = "Idle"
    case arming = "Arming"
    case capturing = "Capturing"
    case collecting = "Collecting"
    case aligning = "Aligning"
    case merging = "Merging"
    case rendering = "Rendering"
    case saving = "Saving"
    case failed = "Failed"
}
