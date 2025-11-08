import Foundation

// Public type aliases for opaque pointers to make them accessible to internal methods
public typealias FileNodePtr = UnsafeMutableRawPointer
public typealias FolderSizeResultPtr = UnsafeMutableRawPointer
    
// Load the library dynamically
private func loadLibrary(_ libraryName: String) -> UnsafeMutableRawPointer? {
    // Try to load from the app bundle first
    
    let libURL = Bundle.main.privateFrameworksURL?.appendingPathComponent("fzc.dylib")
    if let bundlePath = libURL?.path, let handle = dlopen(bundlePath, RTLD_LAZY | RTLD_LOCAL) {
        return handle
    }
    print("Failed to load library from dylib.")
    
    // Try to load from standard locations
    if let handle = dlopen("lib\(libraryName).dylib", RTLD_NOW) {
        return handle
    }
    
    print("Failed to load library: \(String(cString: dlerror()))")
    return nil
}

// Define C function signatures
private func getSymbol<T>(_ libraryHandle: UnsafeMutableRawPointer?,  _ name: String) -> T? {
    guard let handle = libraryHandle,
          let symbol = dlsym(handle, name) else {
        print("Failed to load symbol \(name)")
        return nil
    }
    return unsafeBitCast(symbol, to: T.self)
}

private let FZCLibraryHandle = loadLibrary("fzc")

private class FZCLoader {
    // Renamed C function references to avoid naming conflicts
    static let c_calculateFolderSizes: (@convention(c) (UnsafePointer<CChar>, Bool, Bool, Bool) -> FolderSizeResultPtr?)? = {
        getSymbol(FZCLibraryHandle, "calculateFolderSizes")
    }()
    static let c_getResultRootNode: (@convention(c) (FolderSizeResultPtr?) -> FileNodePtr?)? = {
        getSymbol(FZCLibraryHandle, "getResultRootNode")
    }()
    static let c_getResultElapsedTimeMs: (@convention(c) (FolderSizeResultPtr?) -> Double)? = {
        getSymbol(FZCLibraryHandle, "getResultElapsedTimeMs")
    }()
    static let c_getNodePath: (@convention(c) (FileNodePtr?) -> UnsafePointer<CChar>?)? = {
        getSymbol(FZCLibraryHandle, "getNodePath")
    }()
    static let c_getNodeSize: (@convention(c) (FileNodePtr?) -> UInt64)? = {
        getSymbol(FZCLibraryHandle, "getNodeSize")
    }()
    static let c_isNodeDirectory: (@convention(c) (FileNodePtr?) -> Bool)? = {
        getSymbol(FZCLibraryHandle, "isNodeDirectory")
    }()
    static let c_getChildrenCount: (@convention(c) (FileNodePtr?) -> Int32)? = {
        getSymbol(FZCLibraryHandle, "getChildrenCount")
    }()
    static let c_getChildNode: (@convention(c) (FileNodePtr?, Int32) -> FileNodePtr?)? = {
        getSymbol(FZCLibraryHandle, "getChildNode")
    }()
    static let c_releaseFileNode: (@convention(c) (FileNodePtr?) -> Void)? = {
        getSymbol(FZCLibraryHandle, "releaseFileNode")
    }()
    static let c_releaseResult: (@convention(c) (FolderSizeResultPtr?) -> Void)? = {
        getSymbol(FZCLibraryHandle, "releaseResult")
    }()
}

private let logger = Logger("SummaryTools")

// Swift representation of a file node
class FileNode: Hashable, Identifiable {
    let path: String
    let size: UInt64
    let isDirectory: Bool
    let depth: Int
    public private(set) lazy var children: [FileNode] = { self.getChildNodes() }()
    
    public static func == (lhs: FileNode, rhs: FileNode) -> Bool {
        lhs.path == rhs.path
    }
    
    public func hash(into hasher: inout Hasher) {
        hasher.combine(path)
    }
    
    private let isRoot: Bool
    private let nodePtr: FileNodePtr?
    weak var parentNode: FileNode?
    
    // Initialize from C++ node pointer
    public init(nodePtr: FileNodePtr?, parentNode: FileNode?) {
        self.nodePtr = nodePtr
        self.parentNode = parentNode
        self.isRoot = parentNode == nil
        self.depth = (parentNode?.depth ?? -1) + 1
        
        // Extract properties from C++ node
        if let pathPtr = FZCLoader.c_getNodePath?(nodePtr), let path = String(cString: pathPtr, encoding: .utf8) {
            self.path = path
        } else {
            self.path = ""
        }
        
        self.size = FZCLoader.c_getNodeSize?(nodePtr) ?? 0
        self.isDirectory = FZCLoader.c_isNodeDirectory?(nodePtr) ?? false
    }
    
    deinit {
        // Release C++ resources
        FZCLoader.c_releaseFileNode?(nodePtr)
        
        if isRoot {
            logger.log("Node memory released:", path)
        }
    }
    
    private func getChildNodes() -> [FileNode] {
        var childNodes = [FileNode]()
        let count = Int(FZCLoader.c_getChildrenCount?(nodePtr) ?? 0)
        for i in 0..<count {
            if let childPtr = FZCLoader.c_getChildNode?(nodePtr, Int32(i)) {
                let child = FileNode(nodePtr: childPtr, parentNode: self)
                childNodes.append(child)
            }
        }
        return childNodes
    }
}

class FileSizeCalculator {
    struct Result {
        let rootNode: FileNode
        let elapsedTimeMs: Double
        
        init(rootNode: FileNode, elapsedTimeMs: Double) {
            self.rootNode = rootNode
            self.elapsedTimeMs = elapsedTimeMs
        }
    }
    
    private let useParallelProcessing: Bool
    private let maxThreads: Int
    
    private let logger = Logger("FileSizeCalculator")

    public init(useParallelProcessing: Bool = true, maxThreads: Int = 0) {
        self.useParallelProcessing = useParallelProcessing
        self.maxThreads = maxThreads
    }
    
    func calculate(
        at path: String,
        rootOnly: Bool = false,
        useAllocatedSize: Bool = true,
        includeDirectorySize: Bool = true
    ) -> Result? {
        guard FileManager.default.fileExists(atPath: path) else {
            logger.log("File does not exist at \(path)")
            return nil
        }
        guard let cPath = path.cString(using: .utf8) else {
            logger.log("Failed to convert path to C string")
            return nil
        }
        
        logger.log("Calculator started for \(path)")
        guard
            let standardFunc = FZCLoader.c_calculateFolderSizes,
            let getResultRootNodeFunc = FZCLoader.c_getResultRootNode,
            let getResultElapsedTimeMsFunc = FZCLoader.c_getResultElapsedTimeMs
        else {
            logger.log("C++ functions not available")
            return nil
        }
        
        // Choose the appropriate C function based on configuration
        let resultPtr = standardFunc(cPath, rootOnly, useAllocatedSize, includeDirectorySize)

        // Process the result pointer
        guard let ptr = resultPtr else {
            logger.log("Failed to obtain result ptr")
            return nil
        }
        
        defer {
            // Release the result pointer when done
            FZCLoader.c_releaseResult?(ptr)
        }
        
        let elapsedTimeMs = getResultElapsedTimeMsFunc(ptr)
        if let nodePtr = getResultRootNodeFunc(ptr), FileManager.default.fileExists(atPath: path) {
            let rootNode = FileNode(nodePtr: nodePtr, parentNode: nil)
            return Result(rootNode: rootNode, elapsedTimeMs: elapsedTimeMs)
        }
        
        logger.log("Failed to obtain result node")
        return nil
    }
}
