import Foundation

struct CopyTestResult {
    var directoryStructureMatch: Bool = true
    var contentMatch: Bool = true
    var sizeMatch: Bool = true
    var timestampsMatch: Bool = true
    var errors: [String] = []
}

func validateCopy(source: URL, destination: URL) -> CopyTestResult {
    var result = CopyTestResult()
    
    // 获取源目录的所有内容
    guard let sourceEnum = FileManager.default.enumerator(at: source, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey, .creationDateKey, .contentModificationDateKey]) else {
        result.errors.append("无法读取源目录")
        return result
    }
    
    for case let sourceURL as URL in sourceEnum {
        // 构建对应的目标路径
        let relativePath = sourceURL.relativePathFrom(source)
        let destinationURL = destination.appendingPathComponent(relativePath)
        
        // 检查文件是否存在
        if !FileManager.default.fileExists(atPath: destinationURL.path) {
            result.directoryStructureMatch = false
            result.errors.append("目标文件不存在: \(relativePath)")
            continue
        }
        
        do {
            let sourceAttrs = try sourceURL.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey, .creationDateKey, .contentModificationDateKey])
            let destAttrs = try destinationURL.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey, .creationDateKey, .contentModificationDateKey])
            
            // 跳过目录的内容比较
            if sourceAttrs.isDirectory ?? false {
                continue
            }
            
            // 检查文件大小
            if sourceAttrs.fileSize != destAttrs.fileSize {
                result.sizeMatch = false
                result.errors.append("文件大小不匹配: \(relativePath)")
            }
            
            // 检查时间戳 - 格式化到秒级后比较
            let formatter = DateFormatter()
            formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
            
            let sourceCreationStr = formatter.string(from: sourceAttrs.creationDate ?? Date())
            let destCreationStr = formatter.string(from: destAttrs.creationDate ?? Date())
            let sourceModificationStr = formatter.string(from: sourceAttrs.contentModificationDate ?? Date())
            let destModificationStr = formatter.string(from: destAttrs.contentModificationDate ?? Date())
            
            if sourceCreationStr != destCreationStr || sourceModificationStr != destModificationStr {
                result.timestampsMatch = false
                let message = """
                时间戳不匹配: \(relativePath)
                - 创建时间:
                    源: \(sourceCreationStr)
                    目标: \(destCreationStr)
                - 修改时间:
                    源: \(sourceModificationStr)
                    目标: \(destModificationStr)
                """
                result.errors.append(message)
            }
            
        } catch {
            result.errors.append("比较文件属性失败: \(relativePath), 错误: \(error)")
        }
    }
    
    return result
}

// 打印测试结果
func printTestResult(_ result: CopyTestResult) {
    print("""
    === 复制验证结果 ===
    目录结构匹配: \(result.directoryStructureMatch ? "✅" : "❌")
    内容匹配: \(result.contentMatch ? "✅" : "❌")
    大小匹配: \(result.sizeMatch ? "✅" : "❌")
    时间戳匹配: \(result.timestampsMatch ? "✅" : "❌")
    
    发现的问题:
    \(result.errors.isEmpty ? "无" : result.errors.joined(separator: "\n"))
    ==================
    """)
}

// Helper extension for relative path calculation
private extension URL {
    func relativePathFrom(_ base: URL) -> String {
        let sourcePath = self.path
        let basePath = base.path
        if sourcePath.hasPrefix(basePath) {
            let index = sourcePath.index(sourcePath.startIndex, offsetBy: basePath.count)
            return String(sourcePath[index...]).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        }
        return self.lastPathComponent
    }
}

// 使用示例:
/*
do {
    try fastCopy(sourceURL: sourceURL, destinationURL: destURL, progress: { progress, speed in
        // 处理进度
    }, cancellationToken: token)
    
    let testResult = validateCopy(source: sourceURL, destination: destURL)
    printTestResult(testResult)
} catch {
    print("复制失败: \(error)")
}
*/
