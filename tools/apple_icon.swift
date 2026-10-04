import AppKit
// Resize the original ringworld artwork for the iOS asset catalog.
let directory=CommandLine.arguments[1]
let source=NSImage(contentsOfFile:"port/apple/RingArtwork.png")!
for size in [20,29,40,58,60,76,80,87,120,152,167,180,1024] {
 let bitmap=NSBitmapImageRep(bitmapDataPlanes:nil,pixelsWide:size,pixelsHigh:size,bitsPerSample:8,samplesPerPixel:3,hasAlpha:false,isPlanar:false,colorSpaceName:.deviceRGB,bytesPerRow:0,bitsPerPixel:0)!
 NSGraphicsContext.saveGraphicsState()
 NSGraphicsContext.current=NSGraphicsContext(bitmapImageRep:bitmap)
 NSGraphicsContext.current?.imageInterpolation = .high
 source.draw(in:NSRect(x:0,y:0,width:size,height:size))
 NSGraphicsContext.restoreGraphicsState()
 try bitmap.representation(using:.png,properties:[:])!.write(to:URL(fileURLWithPath:directory+"/AppIcon-\(size).png"))
}
