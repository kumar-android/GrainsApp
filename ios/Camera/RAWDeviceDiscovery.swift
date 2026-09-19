import AVFoundation

struct RAWFormatDescriptor {
    let pixelFormat: OSType
    let dimensions: CMVideoDimensions
    let maxPhotoDimensions: CMVideoDimensions?
    let description: String
}

final class RAWDeviceDiscovery {
    func mainWideCamera() throws -> AVCaptureDevice {
        let session = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInWideAngleCamera, .builtInDualWideCamera],
            mediaType: .video,
            position: .back
        )
        guard let camera = session.devices.first(where: { $0.deviceType == .builtInWideAngleCamera }) ?? session.devices.first else {
            throw NSError(domain: "GCamCamera", code: 1, userInfo: [NSLocalizedDescriptionKey: "No rear physical camera is available"])
        }
        return camera
    }

    func rawFormats(for output: AVCapturePhotoOutput) -> [RAWFormatDescriptor] {
        output.availableRawPhotoPixelFormatTypes.compactMap { type in
            guard AVCapturePhotoOutput.isBayerRAWPixelFormat(type) else { return nil }
            var dimensions = CMVideoDimensions(width: 0, height: 0)
            if let description = output.connections.first?.inputPorts.first?.formatDescription {
                dimensions = CMVideoFormatDescriptionGetDimensions(description)
            }
            return RAWFormatDescriptor(
                pixelFormat: type,
                dimensions: dimensions,
                maxPhotoDimensions: nil,
                description: "fourCC=\(type), dimensions=\(dimensions.width)x\(dimensions.height)"
            )
        }
    }

    func highestQualityRAWType(from output: AVCapturePhotoOutput) throws -> OSType {
        let types = output.availableRawPhotoPixelFormatTypes.filter(AVCapturePhotoOutput.isBayerRAWPixelFormat)
        guard let selected = types.first else {
            throw NSError(domain: "GCamCamera", code: 2, userInfo: [NSLocalizedDescriptionKey: "The selected camera does not expose Bayer RAW"])
        }
        return selected
    }
}
