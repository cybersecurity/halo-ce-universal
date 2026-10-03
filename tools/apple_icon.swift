import AppKit
let directory=CommandLine.arguments[1]
for size in [20,29,40,58,60,76,80,87,120,152,167,180,1024] {
 let image=NSImage(size:NSSize(width:size,height:size));image.lockFocus()
 NSColor(srgbRed:0.04,green:0.09,blue:0.13,alpha:1).setFill();NSRect(x:0,y:0,width:size,height:size).fill()
 let scale=CGFloat(size)/1024
 let path=NSBezierPath();path.move(to:NSPoint(x:260*scale,y:280*scale));path.line(to:NSPoint(x:512*scale,y:730*scale));path.line(to:NSPoint(x:764*scale,y:280*scale));path.lineWidth=85*scale;path.lineCapStyle = .round;path.lineJoinStyle = .round
 NSColor(srgbRed:0.28,green:0.85,blue:0.72,alpha:1).setStroke();path.stroke()
 image.unlockFocus();let bitmap=NSBitmapImageRep(data:image.tiffRepresentation!)!
 try bitmap.representation(using:.png,properties:[:])!.write(to:URL(fileURLWithPath:directory+"/AppIcon-\(size).png"))
}
