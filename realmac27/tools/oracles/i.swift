import AppKit
for a in CommandLine.arguments.dropFirst() {
  let img = NSWorkspace.shared.icon(forFile: a); img.size = NSSize(width: 256, height: 256)
  let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 256, pixelsHigh: 256, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
  NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
  img.draw(in: NSRect(x: 0, y: 0, width: 256, height: 256)); NSGraphicsContext.restoreGraphicsState()
  let n = (a as NSString).lastPathComponent
  try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "/Users/Shared/ic/\(n).png"))
}
