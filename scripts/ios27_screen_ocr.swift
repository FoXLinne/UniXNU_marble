import Foundation
import ImageIO
import Vision

guard CommandLine.arguments.count == 2 else {
    fatalError("Usage: screen-ocr IMAGE.png")
}
let url = URL(fileURLWithPath: CommandLine.arguments[1])
guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    fatalError("Screenshot could not be read")
}
let request = VNRecognizeTextRequest()
request.recognitionLevel = .accurate
request.recognitionLanguages = ["zh-Hans", "en-US"]
request.usesLanguageCorrection = false
try VNImageRequestHandler(cgImage: image).perform([request])
let rows: [[String: Any]] = (request.results ?? []).compactMap { observation in
    guard let text = observation.topCandidates(1).first else { return nil }
    let box = observation.boundingBox
    return ["text": text.string, "confidence": text.confidence,
            "x": box.midX * Double(image.width),
            "y": (1 - box.midY) * Double(image.height),
            "width": box.width * Double(image.width),
            "height": box.height * Double(image.height)]
}
FileHandle.standardOutput.write(try JSONSerialization.data(withJSONObject: rows, options: [.sortedKeys]))
FileHandle.standardOutput.write(Data("\n".utf8))
