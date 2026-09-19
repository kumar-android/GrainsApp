import SwiftUI
import AVFoundation
import UIKit

struct CameraView: View {
    @StateObject private var controller = CameraController()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            CameraPreview(session: controller.captureSession)
                .ignoresSafeArea()

            LinearGradient(
                colors: [.black.opacity(0.55), .clear, .black.opacity(0.78)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()
            .allowsHitTesting(false)

            VStack(spacing: 0) {
                topBar
                Spacer()
                if !controller.lastError.isEmpty { errorBanner }
                bottomBar
            }
            .padding(.horizontal, 18)
            .padding(.top, 10)
            .padding(.bottom, 12)
        }
        .preferredColorScheme(.dark)
        .statusBarHidden(true)
        .task { controller.startPreview() }
        .onDisappear { controller.stopPreview() }
    }

    private var topBar: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text("Computational Lab")
                    .font(.headline.weight(.semibold))
                Text(controller.isReady ? "RAW camera ready" : controller.state.rawValue)
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.72))
            }
            Spacer()
            Menu {
                ForEach(CameraController.mergeFrameOptions, id: \.self) { count in
                    Button {
                        controller.mergeFrameCount = count
                    } label: {
                        if controller.mergeFrameCount == count {
                            Label("\(count) frames", systemImage: "checkmark")
                        } else {
                            Text("\(count) frames")
                        }
                    }
                }
                Divider()
                // Highlight recovery is the second half of the same decision, so it lives in the
                // same menu: the burst length and how far it reaches below the metered exposure
                // together decide how much range the merge has to work with.
                ForEach(CameraController.exposureBracketOptions, id: \.self) { stops in
                    Button {
                        controller.exposureBracketStops = stops
                    } label: {
                        if controller.exposureBracketStops == stops {
                            Label(bracketTitle(stops), systemImage: "checkmark")
                        } else {
                            Text(bracketTitle(stops))
                        }
                    }
                }
            } label: {
                VStack(alignment: .trailing, spacing: 1) {
                    Label("\(controller.mergeFrameCount) frames", systemImage: "square.stack.3d.down.right")
                        .font(.caption.weight(.bold))
                    Text(bracketShortTitle(controller.exposureBracketStops))
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.78))
                }
                .padding(.horizontal, 11)
                .padding(.vertical, 6)
                .background(.black.opacity(0.42), in: Capsule())
            }
            .disabled(!canCapture)
            .accessibilityLabel("Merge frames and highlight recovery")
            Label("RAW", systemImage: "camera.aperture")
                .font(.caption.weight(.bold))
                .padding(.horizontal, 11)
                .padding(.vertical, 7)
                .background(.black.opacity(0.42), in: Capsule())
        }
        .foregroundStyle(.white)
    }

    private func bracketTitle(_ stops: Float) -> String {
        stops <= 0 ? "Single exposure" : String(format: "%.1f EV bracket", stops)
    }

    private func bracketShortTitle(_ stops: Float) -> String {
        stops <= 0 ? "single exposure" : String(format: "HDR %.1f EV", stops)
    }

    private var bottomBar: some View {
        VStack(spacing: 18) {
            if controller.state != .idle, controller.state != .failed {
                HStack(spacing: 9) {
                    ProgressView().tint(.white)
                    Text(controller.state.rawValue)
                        .font(.subheadline.weight(.medium))
                    Spacer()
                }
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(.black.opacity(0.42), in: Capsule())
            }

            HStack(alignment: .center) {
                Button { controller.exportDebugBurst() } label: {
                    VStack(spacing: 5) {
                        Image(systemName: "externaldrive")
                            .font(.title3)
                        Text("Full RAW")
                            .font(.caption2.weight(.semibold))
                    }
                    .frame(width: 74, height: 58)
                }
                .disabled(!canCapture)
                .opacity(canCapture ? 1 : 0.45)

                Spacer()

                Button { controller.captureNaturalBurst() } label: {
                    ZStack {
                        Circle().fill(.white).frame(width: 78, height: 78)
                        Circle().stroke(.black.opacity(0.7), lineWidth: 3).frame(width: 66, height: 66)
                        if controller.state != .idle && controller.state != .failed {
                            ProgressView().tint(.black)
                        }
                    }
                }
                .disabled(!canCapture)
                .accessibilityLabel("Capture computational photo")

                Spacer()

                Group {
                    if let data = controller.resultJPEG, let image = UIImage(data: data) {
                        Image(uiImage: image)
                            .resizable()
                            .scaledToFill()
                    } else {
                        Image(systemName: "photo")
                            .font(.title3)
                    }
                }
                .frame(width: 58, height: 58)
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(.white.opacity(0.75), lineWidth: 1))
                .accessibilityLabel("Latest result")
            }
            .foregroundStyle(.white)

            VStack(spacing: 3) {
                if !controller.resultSummary.isEmpty {
                    Label(controller.resultSummary, systemImage: "checkmark.circle")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.white)
                }
                Text("Capture preview  •  export full-resolution RAW for Windows")
                    .font(.caption2)
                    .foregroundStyle(.white.opacity(0.72))
            }
            .multilineTextAlignment(.center)
        }
    }

    private var errorBanner: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(controller.lastError, systemImage: "exclamationmark.triangle.fill")
                .font(.footnote.weight(.medium))
                .fixedSize(horizontal: false, vertical: true)
            if controller.state == .failed {
                Button("Retry camera") {
                    controller.reset()
                    controller.startPreview()
                }
                .font(.footnote.weight(.semibold))
                .buttonStyle(.bordered)
                .tint(.white)
            }
        }
        .foregroundStyle(.white)
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.red.opacity(0.78), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .padding(.bottom, 14)
    }

    private var canCapture: Bool { controller.state == .idle && controller.isReady }
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
