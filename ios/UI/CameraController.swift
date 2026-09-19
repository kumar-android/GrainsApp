import Foundation
import SwiftUI

@MainActor
final class CameraController: ObservableObject {
    @Published private(set) var state: CaptureState = .idle
    @Published private(set) var lastError = ""
    @Published private(set) var diagnostics = ""
    @Published private(set) var resultJPEG: Data?

    private let capture = RAWBurstCapture()

    init() {
        do { try capture.configure() }
        catch { state = .failed; lastError = error.localizedDescription }
    }

    func startPreview() { capture.start() }
    func stopPreview() { capture.stop() }

    func captureNaturalBurst() {
        guard state == .idle else { return }
        state = .arming
        lastError = ""
        Task {
            do {
                state = .capturing
                let frames = try await capture.captureBurst(count: 8)
                state = .collecting
                let profilePath = Bundle.main.path(forResource: "gcam_natural", ofType: "xml") ?? "config/gcam_natural.xml"
                let processor = try GCamProcessor()
                state = .aligning
                let processed = try processor.process(frames: frames, profilePath: profilePath)
                diagnostics = processed.diagnostics
                state = .rendering
                resultJPEG = try ProcessedImageEncoder.jpeg(width: processed.width, height: processed.height, linearRGB: processed.rgb)
                state = .saving
                if let resultJPEG { try await PhotosExporter.saveJPEG(resultJPEG) }
                state = .idle
            } catch {
                lastError = error.localizedDescription
                state = .failed
            }
        }
    }

    func exportDebugBurst() {
        guard state == .idle else { return }
        state = .arming
        Task {
            do {
                state = .capturing
                let frames = try await capture.captureBurst(count: 8)
                state = .saving
                let directory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("GCamRAW_\(Int(Date().timeIntervalSince1970))")
                try RAWPackExporter.write(frames: frames, to: directory)
                state = .idle
            } catch {
                lastError = error.localizedDescription
                state = .failed
            }
        }
    }
}
