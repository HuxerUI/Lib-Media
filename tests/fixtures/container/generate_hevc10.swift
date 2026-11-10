import AVFoundation
import CoreGraphics
import CoreVideo
import Foundation
import VideoToolbox

let file = URL(fileURLWithPath: CommandLine.arguments[1])
let hlg = CommandLine.arguments.dropFirst(2).first == "hlg"
let writer = try AVAssetWriter(outputURL: file, fileType: .mov)
let width = 128, height = 96
var settings: [String: Any] = [
    AVVideoCodecKey: AVVideoCodecType.hevc,
    AVVideoWidthKey: width, AVVideoHeightKey: height,
    AVVideoCompressionPropertiesKey: [
        AVVideoProfileLevelKey: kVTProfileLevel_HEVC_Main10_AutoLevel,
        AVVideoMaxKeyFrameIntervalKey: 6,
        AVVideoAllowFrameReorderingKey: false,
    ],
    AVVideoColorPropertiesKey: [
        AVVideoColorPrimariesKey: hlg ? AVVideoColorPrimaries_ITU_R_2020 : AVVideoColorPrimaries_ITU_R_709_2,
        AVVideoTransferFunctionKey: hlg ? AVVideoTransferFunction_ITU_R_2100_HLG : AVVideoTransferFunction_ITU_R_709_2,
        AVVideoYCbCrMatrixKey: hlg ? AVVideoYCbCrMatrix_ITU_R_2020 : AVVideoYCbCrMatrix_ITU_R_709_2,
    ],
]
if hlg {
    settings[AVVideoPixelAspectRatioKey] = [
        AVVideoPixelAspectRatioHorizontalSpacingKey: 4,
        AVVideoPixelAspectRatioVerticalSpacingKey: 3,
    ]
}
let input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
input.transform = CGAffineTransform(a: 0, b: 1, c: -1, d: 0, tx: CGFloat(height), ty: 0)
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
    kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height,
])
writer.add(input)
guard writer.startWriting() else { fatalError(String(describing: writer.error)) }
writer.startSession(atSourceTime: .zero)
for index in 0..<18 {
    while !input.isReadyForMoreMediaData {
        guard writer.status == .writing else { fatalError(String(describing: writer.error)) }
        Thread.sleep(forTimeInterval: 0.001)
    }
    var buffer: CVPixelBuffer?
    guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, adaptor.pixelBufferPool!, &buffer) == kCVReturnSuccess,
          let buffer else { fatalError("Cannot allocate P010 fixture") }
    CVPixelBufferLockBaseAddress(buffer, [])
    for plane in 0..<2 {
        let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!
        let stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
        for y in 0..<CVPixelBufferGetHeightOfPlane(buffer, plane) {
            let row = base.advanced(by: y * stride).assumingMemoryBound(to: UInt16.self)
            for x in 0..<width {
                let sample = plane == 0 ? 64 + ((x * 3 + y * 5 + index * 17) % 864) : 512
                row[x] = UInt16(sample << 6)
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, [])
    guard adaptor.append(buffer, withPresentationTime: CMTime(value: Int64(index), timescale: 12)) else {
        fatalError(String(describing: writer.error))
    }
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
guard writer.status == .completed else { fatalError(String(describing: writer.error)) }
