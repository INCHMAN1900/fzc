import Foundation

// Import the C functions
@_silgen_name("calculateFolderSizes")
func c_calculateFolderSizes(_ path: UnsafePointer<CChar>, _ useParallelProcessing: Bool, _ maxThreads: Int32) -> UnsafeMutablePointer<folder_size_result_t>?

@_silgen_name("freeFolderSizeResult")
func c_freeFolderSizeResult(_ result: UnsafeMutablePointer<folder_size_result_t>?)

// Swift structures
public struct FileNode {
    public let path: String
    public let size: UInt64
    public let children: [FileNode]
}

public class FolderSizeCalculator {
    public init() {}
    
    public func calculateFolderSizes(path: String, useParallelProcessing: Bool = true, maxThreads: Int32 = 8) throws -> FileNode {
        // Convert the path to UTF-8
        guard let utf8Path = (path as NSString).fileSystemRepresentation else {
            throw NSError(domain: "FolderSizeCalculator", code: -1, 
                         userInfo: [NSLocalizedDescriptionKey: "Failed to convert path to UTF-8"])
        }
        
        guard let result = c_calculateFolderSizes(utf8Path, useParallelProcessing, maxThreads) else {
            throw NSError(domain: "FolderSizeCalculator", code: -1, 
                         userInfo: [NSLocalizedDescriptionKey: "Failed to calculate folder sizes"])
        }
        
        defer {
            c_freeFolderSizeResult(result)
        }
        
        guard let root = result.pointee.root else {
            throw NSError(domain: "FolderSizeCalculator", code: -2, 
                         userInfo: [NSLocalizedDescriptionKey: "No root node found"])
        }
        
        return convertToFileNode(root)
    }
    
    private func convertToFileNode(_ node: UnsafePointer<file_node_t>) -> FileNode {
        // Convert C string to Swift string using UTF-8 encoding
        let path = String(cString: node.pointee.path, encoding: .utf8) ?? ""
        let size = node.pointee.size
        
        var children: [FileNode] = []
        if let childrenPtr = node.pointee.children {
            for i in 0..<Int(node.pointee.num_children) {
                let childPtr = childrenPtr.advanced(by: i)
                children.append(convertToFileNode(childPtr))
            }
        }
        
        return FileNode(path: path, size: size, children: children)
    }
} 