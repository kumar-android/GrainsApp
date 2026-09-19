import Foundation
import SwiftUI
import AVFoundation

@MainActor
final class CameraController: ObservableObject {
    @Published private(set) var state: CaptureState = .idle
    @Published private(set) var lastError = ""
    @Published private(set) var diagnostics = ""
    @Published private(set) var resultJPEG: Data?
    @Published private(set) var isReady = false

    private let capture = RAWBurstCapture()
    private var previewTask: Task<Void, Never>?

    init() {}

    func startPreview() {
        guard !isReady else { capture.start(); return }
        previewTask?.cancel()
        previewTask = Task { @MainActor [weak self] in
            guard let self else { return }
            guard await AVCaptureDevice.requestAccess(for: .video) else {
                lastError = "Camera access was not granted"
                state = .failed
                return
            }
            do {
                try capture.configure()
                isReady = true
                capture.start()
            } catch {
                lastError = error.localizedDescription
                state = .failed
            }
        }
    }

    func stopPreview() {
        previewTask?.cancel()
        capture.stop()
    }

    func reset() {
        guard state == .failed else { return }
        lastError = ""
        state = isReady ? .idle : .failed
    }

    func captureNaturalBurst() {
        guard state == .idle, isReady else { return }
        state = .arming
        lastError = ""
        diagnostics = ""
        Task {
            do {
                state = .capturing
                let frames = try await capture.captureBurst(count: 8)
                state = .collecting
                guard let profilePath = Bundle.main.path(forResource: "gcam_natural", ofType: "xml") else {
                    throw NSError(domain: "GCamCamera", code: 26, userInfo: [NSLocalizedDescriptionKey: "The bundled tuning profile is missing"])
                }
                state = .aligning
                let processed = try await Task.detached(priority: .userInitiated) {
                    let processor = try GCamProcessor()
                    return try processor.process(frames: frames, profilePath: profilePath)
                }.value
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
        guard state == .idle, isReady else { return }
        state = .arming
        Task {
            do {
                state = .capturing
                let frames = try await capture.captureBurst(count: 8)
                state = .saving
                let directory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("GCamRAW_\(Int(Date().timeIntervalSince1970))")
                try await Task.detached(priority: .utility) {
                    try RAWPackExporter.write(frames: frames, to: directory)
                }.value
                state = .idle
            } catch {
                lastError = error.localizedDescription
                state = .failed
            }
        }
    }
}
