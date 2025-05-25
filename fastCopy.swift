import Foundation
import System
import AppKit

// 文件复制任务结构
private struct CopyTask {
    let source: URL
    let destination: URL
    let size: Int64
}

// 用于进度追踪的结构
private class ProgressTracker {
    private let progressCallback: (Double, Int64) -> Void
    var totalBytes: Int64 = 0
    private(set) var copiedBytes: Int64 = 0
    private var lastSpeedUpdateTime: TimeInterval
    private var lastTotalCopiedBytes: Int64 = 0
    private var currentSpeed: Int64 = 0
    private let updateInterval: TimeInterval = 0.5
    
    private let formatter: ByteCountFormatter = {
        let f = ByteCountFormatter()
        f.countStyle = .binary
        f.allowedUnits = [.useAll]
        f.includesUnit = true
        f.isAdaptive = true
        return f
    }()
    
    init(progressCallback: @escaping (Double, Int64) -> Void) {
        self.progressCallback = progressCallback
        lastSpeedUpdateTime = CACurrentMediaTime()
        lastTotalCopiedBytes = 0
    }
    
    private func formatSize(_ bytes: Int64) -> String {
        return formatter.string(fromByteCount: bytes)
    }
    
    func updateProgress(copied: Int64) {
        copiedBytes += copied
        let now = CACurrentMediaTime()
        let timeDiff = now - lastSpeedUpdateTime
        
        if timeDiff >= updateInterval {
            let totalBytesDiff = copiedBytes - lastTotalCopiedBytes
            if timeDiff > 0 {
                currentSpeed = Int64(Double(totalBytesDiff) / timeDiff)
                lastSpeedUpdateTime = now
                lastTotalCopiedBytes = copiedBytes
                notifyProgress()
            }
        }
    }
    
    private func notifyProgress() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            let percentage = self.totalBytes > 0 ? Double(self.copiedBytes) / Double(self.totalBytes) : 0
            self.progressCallback(percentage, self.currentSpeed)
        }
    }
}

// 根据文件大小选择合适的缓冲区大小
private func chooseBSize(fileSize: Int64) -> Int {
    switch fileSize {
    case 0..<1024*1024:         // < 1MB
        return 256*1024         // 256KB
    case 1024*1024..<100*1024*1024:  // 1MB-100MB
        return 4*1024*1024      // 4MB
    default:                    // >= 100MB
        return 16*1024*1024     // 16MB
    }
}

// 主复制函数
private enum CopyItem {
    case directory(source: URL, destination: URL, attributes: [FileAttributeKey: Any])
    case file(source: URL, destination: URL, size: Int64)
}

func fastCopy(
    sourceURL: URL,
    destinationURL: URL,
    progress: @escaping (_ copied: Double, _ speed: Int64) -> Void,
    cancellationToken: CancellationToken
) throws {
    let startTime = CACurrentMediaTime()
    var timeMarker = startTime
    let formatter = ByteCountFormatter()
    
    func logPhase(_ phase: String) {
        let now = CACurrentMediaTime()
        let duration = now - timeMarker
        print("Phase '\(phase)' took: \(String(format: "%.2fs", duration))")
        timeMarker = now
    }
    
    let tracker = ProgressTracker(progressCallback: progress)
    
    // 检查源文件
    var isDirectory: ObjCBool = false
    guard FileManager.default.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory) else {
        throw CopyError.sourceNotFound
    }
    logPhase("Initial check")
    
    if !isDirectory.boolValue {
        // 处理单文件复制的情况
        let fileName = sourceURL.lastPathComponent
        let destPath = FileManager.default.fileExists(atPath: destinationURL.path, isDirectory: &isDirectory) && isDirectory.boolValue
            ? destinationURL.appendingPathComponent(fileName)  // 目标是文件夹，添加文件名
            : destinationURL  // 目标是文件，直接使用
        
        let size = (try? FileManager.default.attributesOfItem(atPath: sourceURL.path)[.size] as? Int64) ?? 0
        tracker.totalBytes = size
        try copyFile(
            source: sourceURL,
            destination: destPath,
            size: size,
            bufferSize: chooseBSize(fileSize: size),
            tracker: tracker,
            progress: progress,
            cancellationToken: cancellationToken
        )
        return
    }
    
    // 扫描阶段：收集目录和文件信息
    var tasks: [CopyItem] = []
    var totalSize: Int64 = 0
    
    if let enumerator = FileManager.default.enumerator(at: sourceURL, includingPropertiesForKeys: [.fileSizeKey, .isDirectoryKey]) {
        for case let url as URL in enumerator {
            if cancellationToken.isCancelled { throw CancellationError() }
            
            let attrs = try url.resourceValues(forKeys: [.fileSizeKey, .isDirectoryKey])
            let destPath = destinationURL.appendingPathComponent(url.relativePath(from: sourceURL))
            
            if attrs.isDirectory ?? false {
                // 获取目录的完整属性
                let dirAttrs = try FileManager.default.attributesOfItem(atPath: url.path)
                tasks.append(.directory(source: url, destination: destPath, attributes: dirAttrs))
            } else {
                let size = Int64(attrs.fileSize ?? 0)
                totalSize += size
                tasks.append(.file(source: url, destination: destPath, size: size))
            }
        }
    }
    logPhase("Directory scanning")
    
    // 创建根目录
    try FileManager.default.createDirectory(at: destinationURL, withIntermediateDirectories: true)
    
    // 按路径长度排序，确保父目录先创建
    tasks.sort { item1, item2 in
        switch (item1, item2) {
        case (.directory, .file): return true  // 目录优先
        case (.file, .directory): return false
        case let (.directory(_, dest1, _), .directory(_, dest2, _)):
            return dest1.path.count < dest2.path.count
        default: return false
        }
    }
    
    tracker.totalBytes = totalSize
    let copyStartTime = timeMarker
    
    // 执行任务
    let concurrency = min(ProcessInfo.processInfo.activeProcessorCount * 2, max(4, tasks.count))
    let queue = DispatchQueue(label: "com.fastcopy.queue",
                            qos: .userInitiated,
                            attributes: .concurrent)
    let group = DispatchGroup()
    let semaphore = DispatchSemaphore(value: concurrency)
    
    // 分发任务
    for task in tasks {
        if cancellationToken.isCancelled { throw CancellationError() }
        
        group.enter()
        semaphore.wait()
        
        queue.async(group: group) {
            autoreleasepool {
                do {
                    switch task {
                    case let .directory(_, destination, attributes):
                        try? FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
                        // Restore directory attributes after creation
                        try? FileManager.default.setAttributes(attributes, ofItemAtPath: destination.path)
                    case let .file(source, destination, size):
                        try copyFile(
                            source: source,
                            destination: destination,
                            size: size,
                            bufferSize: chooseBSize(fileSize: size),
                            tracker: tracker,
                            progress: progress,
                            cancellationToken: cancellationToken
                        )
                    }
                } catch {
                    print("Error processing \(task): \(error)")
                }
                semaphore.signal()
                group.leave()
            }
        }
    }
    
    group.wait()
    if cancellationToken.isCancelled { throw CancellationError() }
    
    // 打印统计信息
    let totalTime = CACurrentMediaTime() - startTime
    let copyTime = CACurrentMediaTime() - copyStartTime
    print("""
    --- Copy Statistics ---
    Directories: \(tasks.compactMap { task -> URL? in
        if case .directory(_, let destination, _) = task {
            return destination
        }
        return nil
    }.count)
    Files: \(tasks.compactMap { task -> URL? in
        if case .file(let source, let destination, _) = task {
            return destination
        }
        return nil
    }.count)
    Total size: \(formatter.string(fromByteCount: totalSize))
    Copy time: \(String(format: "%.2fs", copyTime))
    Total time: \(String(format: "%.2fs", totalTime))
    Average speed: \(formatter.string(fromByteCount: Int64(Double(totalSize) / copyTime)))/s
    ------------------
    """)
}

// 复制错误枚举
private enum CopyError: Error {
    case sourceNotFound
    case copyFailed(errno: Int32)
    case cancelled
}

// 复制上下文结构
private class CopyContext {
    let progress: (Double, Int64) -> Void
    let token: CancellationToken
    let tracker: ProgressTracker
    let destination: String
    var lastReportedBytes: Int64 = 0  // Add this field
    
    init(progress: @escaping (Double, Int64) -> Void, token: CancellationToken, tracker: ProgressTracker, destination: String) {
        self.progress = progress
        self.token = token
        self.tracker = tracker
        self.destination = destination
    }
}

// 单个文件复制实现
private func copyFile(
    source: URL,
    destination: URL,
    size: Int64,
    bufferSize: Int,
    tracker: ProgressTracker,
    progress: @escaping (Double, Int64) -> Void,
    cancellationToken: CancellationToken
) throws {
    let sourcePath = source.path
    let destinationPath = destination.path
    
    // 检查源文件是否存在
    guard FileManager.default.fileExists(atPath: sourcePath) else {
        throw CopyError.sourceNotFound
    }
    
    // 确保目标目录存在
    let destinationDir = (destinationPath as NSString).deletingLastPathComponent
    try? FileManager.default.createDirectory(atPath: destinationDir, withIntermediateDirectories: true)
    
    let state = copyfile_state_alloc()
    guard let state = state else {
        throw CopyError.copyFailed(errno: errno)
    }
    defer { copyfile_state_free(state) }
    
    let context = CopyContext(progress: progress, token: cancellationToken, tracker: tracker, destination: destinationPath)
    let contextPtr = Unmanaged.passUnretained(context).toOpaque()
    
    // 设置缓冲区大小
    var localBufferSize = bufferSize
    copyfile_state_set(state, UInt32(COPYFILE_STATE_BSIZE), &localBufferSize)
    
    // 设置回调和上下文
    let callback: @convention(c) (Int32, Int32, copyfile_state_t?, UnsafePointer<Int8>?, UnsafePointer<Int8>?, UnsafeMutableRawPointer?) -> Int32 = { what, stage, state, src, dst, ctx in
        guard let ctx = ctx else { return 0 }
        let context = Unmanaged<CopyContext>.fromOpaque(ctx).takeUnretainedValue()
        
        if context.token.isCancelled {
            return Int32(COPYFILE_QUIT)
        }
        
        if what == COPYFILE_PROGRESS, let state = state {
            var currentFileBytes: off_t = 0
            copyfile_state_get(state, UInt32(COPYFILE_STATE_COPIED), &currentFileBytes)
            
            // Use context's lastReportedBytes
            let increment = Int64(currentFileBytes) - context.lastReportedBytes
            if increment > 0 {
                context.lastReportedBytes = Int64(currentFileBytes)
                context.tracker.updateProgress(copied: increment)
            }
        }
        return 0
    }
    
    copyfile_state_set(state, UInt32(COPYFILE_STATE_STATUS_CB), unsafeBitCast(callback, to: UnsafeRawPointer.self))
    copyfile_state_set(state, UInt32(COPYFILE_STATE_STATUS_CTX), contextPtr)
    
    // 只复制数据，不复制其他属性以提高速度
    // 修改: 使用 COPYFILE_ALL 复制所有属性，包括时间戳、权限等
    // COPYFILE_NOFOLLOW 避免复制符号链接指向的文件
    let flags = UInt32(COPYFILE_ALL | COPYFILE_NOFOLLOW | COPYFILE_METADATA | COPYFILE_XATTR)
    
    let result = sourcePath.withCString { src in
        destinationPath.withCString { dst in
            copyfile(src, dst, state, flags)
        }
    }
    
    if result != 0 {
        if cancellationToken.isCancelled {
            throw CopyError.cancelled
        } else {
            throw CopyError.copyFailed(errno: errno)
        }
    }
    
    // 手动恢复文件的所有属性
    if let sourceAttrs = try? FileManager.default.attributesOfItem(atPath: sourcePath) {
        try? FileManager.default.setAttributes(sourceAttrs, ofItemAtPath: destinationPath)
    }
}

// Helper extension for relative path calculation
private extension URL {
    func relativePath(from base: URL) -> String {
        let sourcePath = self.path
        let basePath = base.path
        if sourcePath.hasPrefix(basePath) {
            let index = sourcePath.index(sourcePath.startIndex, offsetBy: basePath.count)
            return String(sourcePath[index...]).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        }
        return self.lastPathComponent
    }
}
