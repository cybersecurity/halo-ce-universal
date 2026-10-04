import AppKit
import ImageIO
import UniformTypeIdentifiers
// Draw through CoreGraphics: NSImage drawing into an offscreen AppKit context
// can silently produce black output when this command runs without an app.
let directory = CommandLine.arguments[1]
let sourceURL = URL(fileURLWithPath: "port/apple/RingArtwork.png")
let source = CGImageSourceCreateImageAtIndex(CGImageSourceCreateWithURL(sourceURL as CFURL, nil)!, 0, nil)!
func writeIcon(size:Int, url:URL) {
    let context = CGContext(data:nil,width:size,height:size,bitsPerComponent:8,bytesPerRow:size*4,
        space:CGColorSpaceCreateDeviceRGB(),bitmapInfo:CGImageAlphaInfo.noneSkipLast.rawValue)!
    context.interpolationQuality = .high
    context.draw(source,in:CGRect(x:0,y:0,width:size,height:size))
    let rendered = context.makeImage()!
    let output = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(output, rendered, nil)
    precondition(CGImageDestinationFinalize(output))
}

for size in [20,29,40,58,60,76,80,87,120,152,167,180,1024] {
    writeIcon(size:size,url:URL(fileURLWithPath:directory+"/AppIcon-\(size).png"))
}
// Optional second output is the macOS .icns file.
if CommandLine.arguments.count > 2 {
    let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    let iconset = temporary.appendingPathComponent("Community.iconset")
    try FileManager.default.createDirectory(at:iconset,withIntermediateDirectories:true)
    defer { try? FileManager.default.removeItem(at:temporary) }
    for size in [16,32,128,256,512] {
        for scale in [1,2] {
            let suffix = scale == 2 ? "@2x" : ""
            writeIcon(size:size*scale,url:iconset.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"))
        }
    }
    let process = Process();process.executableURL=URL(fileURLWithPath:"/usr/bin/iconutil")
    process.arguments=["-c","icns",iconset.path,"-o",CommandLine.arguments[2]]
    try process.run();process.waitUntilExit();precondition(process.terminationStatus == 0)
}
