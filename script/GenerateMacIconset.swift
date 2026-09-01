import AppKit
import Foundation

guard CommandLine.arguments.count == 3 else {
    fputs("用法：GenerateMacIconset.swift <源 PNG> <输出 iconset 目录>\n", stderr)
    exit(2)
}

let sourcePath = CommandLine.arguments[1]
let outputDirectory = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true)
guard let sourceImage = NSImage(contentsOfFile: sourcePath) else {
    fputs("无法读取图标源文件：\(sourcePath)\n", stderr)
    exit(3)
}

let variants: [(Int, String)] = [
    (16, "icon_16x16.png"),
    (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"),
    (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"),
    (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"),
    (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"),
    (1024, "icon_512x512@2x.png"),
]

// Legacy .icns files are rendered as precomposed artwork. Keep the visible
// rounded rectangle inside the macOS icon grid instead of stretching it to
// the full bitmap like a Windows icon. The source already contains a small
// transparent edge. Because this dark, high-contrast artwork has more visual
// weight than light system icons, use an 11% canvas inset on every side; its
// visible footprint is then roughly 71% of the 1024px canvas.
let macOSCanvasInsetRatio = 0.11

try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
for (pixels, filename) in variants {
    guard let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: pixels,
        pixelsHigh: pixels,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 32
    ) else {
        fputs("无法创建 \(pixels)px 图标画布。\n", stderr)
        exit(4)
    }

    NSGraphicsContext.saveGraphicsState()
    guard let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
        NSGraphicsContext.restoreGraphicsState()
        fputs("无法创建图标绘制上下文。\n", stderr)
        exit(5)
    }
    NSGraphicsContext.current = context
    context.imageInterpolation = .high
    let inset = CGFloat(pixels) * macOSCanvasInsetRatio
    sourceImage.draw(
        in: NSRect(x: inset, y: inset,
                   width: CGFloat(pixels) - inset * 2,
                   height: CGFloat(pixels) - inset * 2),
        from: NSRect(origin: .zero, size: sourceImage.size),
        operation: .copy,
        fraction: 1.0
    )
    context.flushGraphics()
    NSGraphicsContext.restoreGraphicsState()

    guard let png = bitmap.representation(using: .png, properties: [:]) else {
        fputs("无法编码 \(filename)。\n", stderr)
        exit(6)
    }
    try png.write(to: outputDirectory.appendingPathComponent(filename), options: .atomic)
}
