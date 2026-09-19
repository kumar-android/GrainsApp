import SwiftUI
import AVFoundation
import UIKit

struct CameraView: View {
    @StateObject private var controller = CameraController()

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    CameraPreview(session: controller.captureSession)
                        .frame(maxWidth: .infinity)
                        .aspectRatio(3.0 / 4.0, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
                        .overlay(alignment: .topLeading) {
                            Label(controller.state.rawValue, systemImage: statusIcon)
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(statusColor)
                                .padding(.horizontal, 12)
                                .padding(.vertical, 8)
                                .background(.ultraThinMaterial, in: Capsule())
                                .padding(14)
                        }

                    VStack(alignment: .leading, spacing: 8) {
                        Text("Natural RAW burst")
                            .font(.title2.weight(.semibold))
                        Text("Capture a short Bayer burst and render a memory-safe on-device preview. Use RAW export for the full-resolution burst and Windows reference processing.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }

                    HStack(spacing: 12) {
                        Button { controller.captureNaturalBurst() } label: {
                            Label("Capture preview", systemImage: "camera.aperture")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(controller.state != .idle || !controller.isReady)

                        Button { controller.exportDebugBurst() } label: {
                            Label("Export full RAW", systemImage: "externaldrive")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                        .disabled(controller.state != .idle || !controller.isReady)
                    }

                    if controller.state != .idle, controller.state != .failed {
                        HStack(spacing: 10) {
                            ProgressView()
                            Text(controller.state.rawValue)
                                .font(.subheadline.weight(.medium))
                            Spacer()
                        }
                        .foregroundStyle(.secondary)
                        .padding(.vertical, 2)
                    }

                    if let resultJPEG = controller.resultJPEG, let image = UIImage(data: resultJPEG) {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Latest result").font(.headline)
                            Image(uiImage: image)
                                .resizable()
                                .scaledToFit()
                                .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                        }
                    }

                    if !controller.lastError.isEmpty {
                        Label(controller.lastError, systemImage: "exclamationmark.triangle.fill")
                            .font(.footnote)
                            .foregroundStyle(.red)
                            .padding(12)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(.red.opacity(0.10), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                    }

                    if controller.state == .failed {
                        Button("Retry camera setup") { controller.reset(); controller.startPreview() }
                            .buttonStyle(.bordered)
                    }

                    if !controller.diagnostics.isEmpty {
                        DisclosureGroup("Processing diagnostics") {
                            Text(controller.diagnostics)
                                .font(.caption.monospaced())
                                .textSelection(.enabled)
                        }
                        .padding(.top, 4)
                    }
                }
                .padding()
            }
            .navigationTitle("Computational Lab")
            .navigationBarTitleDisplayMode(.inline)
            .background(Color(uiColor: .systemGroupedBackground))
            .task { controller.startPreview() }
            .onDisappear { controller.stopPreview() }
        }
    }

    private var statusIcon: String {
        if controller.state == .failed { return "exclamationmark.circle.fill" }
        return controller.isReady ? "checkmark.circle.fill" : "circle.dashed"
    }

    private var statusColor: Color {
        if controller.state == .failed { return .red }
        return controller.isReady ? .green : .secondary
    }
}

private struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer?.session = session
        view.previewLayer?.videoGravity = .resizeAspectFill
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {
        if let previewLayer = uiView.previewLayer, previewLayer.session !== session {
            previewLayer.session = session
        }
    }
}

private final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }

    var previewLayer: AVCaptureVideoPreviewLayer? { layer as? AVCaptureVideoPreviewLayer }
}
