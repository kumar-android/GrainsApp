import SwiftUI

struct CameraView: View {
    @StateObject private var controller = CameraController()
    @State private var mode = "Natural"

    var body: some View {
        VStack(spacing: 16) {
            Text("GCam Computational Lab").font(.headline)
            Text(controller.state.rawValue).font(.caption).foregroundStyle(.secondary)
            Picker("Capture mode", selection: $mode) {
                ForEach(["Natural", "HDR+", "High Detail", "Low Light"], id: \.self, content: Text.init)
            }.pickerStyle(.segmented)
            HStack {
                Button("Capture RAW burst") { controller.captureNaturalBurst() }
                    .buttonStyle(.borderedProminent)
                    .disabled(controller.state != .idle || !controller.isReady)
                Button("Debug export") { controller.exportDebugBurst() }
                    .buttonStyle(.bordered)
                    .disabled(controller.state != .idle || !controller.isReady)
            }
            if controller.state == .failed {
                Button("Retry") { controller.reset(); controller.startPreview() }
                    .buttonStyle(.bordered)
            }
            if !controller.lastError.isEmpty { Text(controller.lastError).foregroundStyle(.red).font(.footnote) }
            Text("Final captures use Bayer RAW burst processing. Preview is intentionally separate from the full-quality path.")
                .font(.footnote)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
        }
        .padding()
        .onAppear { controller.startPreview() }
        .onDisappear { controller.stopPreview() }
    }
}
