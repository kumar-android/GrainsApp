import Photos
import UIKit

enum PhotosExporter {
    static func saveJPEG(_ data: Data) async throws {
        let authorization = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
        guard authorization == .authorized || authorization == .limited else {
            throw NSError(domain: "GCamCamera", code: 41, userInfo: [NSLocalizedDescriptionKey: "Photo library write access was not granted"])
        }
        try await withCheckedThrowingContinuation { continuation in
            PHPhotoLibrary.shared().performChanges {
                let request = PHAssetCreationRequest.forAsset()
                request.addResource(with: .photo, data: data, options: nil)
            } completionHandler: { success, error in
                if let error { continuation.resume(throwing: error) }
                else if success { continuation.resume() }
                else { continuation.resume(throwing: NSError(domain: "GCamCamera", code: 40, userInfo: [NSLocalizedDescriptionKey: "Photos did not save the result"])) }
            }
        }
    }
}
