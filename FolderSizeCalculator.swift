import Foundation

// Define the C function interfaces
private let libraryName = "folder_size_calculator"

// Load the library dynamically
private func loadLibrary() -> UnsafeMutableRawPointer? {
    // Try to load from the app bundle first
    if let bundlePath = Bundle.main.path(forResource: libraryName, ofType: "dylib") {
        // Use RTLD_LAZY for more permissive loading and RTLD_LOCAL to avoid symbol conflicts
        if let handle = dlopen(bundlePath, RTLD_LAZY | RTLD_LOCAL) {
            return handle
        } else {
            // If direct path fails, try with @rpath prefix
            let rpathPath = "@rpath/\(libraryName).dylib"
            if let handle = dlopen(rpathPath, RTLD_LAZY | RTLD_LOCAL) {
                return handle
            }
        }
    }
    
    // Try to load from standard locations
    if let handle = dlopen("lib\(libraryName).dylib", RTLD_NOW) {
        return handle
    }
    
    print("Failed to load library: \(String(cString: dlerror()))")
    return nil
}

private let libraryHandle = loadLibrary()

// Define C function signatures
private func getSymbol<T>(_ name: String) -> T? {
    guard let handle = libraryHandle,
          let symbol = dlsym(handle, name) else {
        print("Failed to load symbol \(name)")
        return nil
    }
    return unsafeBitCast(symbol, to: T.self)
}

// Public type aliases for opaque pointers to make them accessible to internal methods
public typealias FileNodePtr = UnsafeMutableRawPointer
public typealias FolderSizeResultPtr = UnsafeMutableRawPointer

// Renamed C function references to avoid naming conflicts
private let c_calculateFolderSizes: (@convention(c) (UnsafePointer<CChar>) -> FolderSizeResultPtr?)? = 
    getSymbol("calculateFolderSizes")
private let c_calculateFolderSizesParallel: (@convention(c) (UnsafePointer<CChar>, Bool, Int32) -> FolderSizeResultPtr?)? = 
    getSymbol("calculateFolderSizesParallel")
private let c_getResultRootNode: (@convention(c) (FolderSizeResultPtr?) -> FileNodePtr?)? = 
    getSymbol("getResultRootNode")
private let c_getResultElapsedTimeMs: (@convention(c) (FolderSizeResultPtr?) -> Double)? = 
    getSymbol("getResultElapsedTimeMs")
private let c_getNodePath: (@convention(c) (FileNodePtr?) -> UnsafePointer<CChar>?)? = 
    getSymbol("getNodePath")
private let c_getNodeSize: (@convention(c) (FileNodePtr?) -> UInt64)? = 
    getSymbol("getNodeSize")
private let c_isNodeDirectory: (@convention(c) (FileNodePtr?) -> Bool)? = 
    getSymbol("isNodeDirectory")
private let c_getChildrenCount: (@convention(c) (FileNodePtr?) -> Int32)? = 
    getSymbol("getChildrenCount")
private let c_getChildNode: (@convention(c) (FileNodePtr?, Int32) -> FileNodePtr?)? = 
    getSymbol("getChildNode")
private let c_releaseFileNode: (@convention(c) (FileNodePtr?) -> Void)? = 
    getSymbol("releaseFileNode")
private let c_releaseResult: (@convention(c) (FolderSizeResultPtr?) -> Void)? = 
    getSymbol("releaseResult")

// Swift representation of a file node
public class FileNode {
    public let path: String
    public let size: UInt64
    public let isDirectory: Bool
    public private(set) var children: [FileNode] = []
    
    private let nodePtr: FileNodePtr?
    
    // Initialize from C++ node pointer
    public init(nodePtr: FileNodePtr?) {
        self.nodePtr = nodePtr
        
        // Extract properties from C++ node
        if let pathPtr = c_getNodePath?(nodePtr), let path = String(cString: pathPtr, encoding: .utf8) {
            self.path = path
        } else {
            self.path = ""
        }
        
        self.size = c_getNodeSize?(nodePtr) ?? 0
        self.isDirectory = c_isNodeDirectory?(nodePtr) ?? false
        
        // Load children
        let count = Int(c_getChildrenCount?(nodePtr) ?? 0)
        for i in 0..<count {
            if let childPtr = c_getChildNode?(nodePtr, Int32(i)) {
                let child = FileNode(nodePtr: childPtr)
                children.append(child)
            }
        }
    }
    
    deinit {
        // Release C++ resources
        c_releaseFileNode?(nodePtr)
    }
}

// Result structure that includes the root node and timing information
public struct FolderSizeResult {
    public let rootNode: FileNode
    public let elapsedTimeMs: Double
    
    public init(rootNode: FileNode, elapsedTimeMs: Double) {
        self.rootNode = rootNode
        self.elapsedTimeMs = elapsedTimeMs
    }
}

// Main Swift interface
public class FolderSizeCalculator {
    private let useParallelProcessing: Bool
    private let maxThreads: Int
    
    public init(useParallelProcessing: Bool = true, maxThreads: Int = 0) {
        self.useParallelProcessing = useParallelProcessing
        self.maxThreads = maxThreads
    }
    
    public func calculateFolderSizes(at path: String) -> FolderSizeResult? {
        guard let cPath = path.cString(using: .utf8) else {
            print("Failed to convert path to C string")
            return nil
        }
        
        // Choose the appropriate C function based on configuration
        let resultPtr: FolderSizeResultPtr?
        
        if useParallelProcessing, let parallelFunc = c_calculateFolderSizesParallel {
            resultPtr = parallelFunc(cPath, useParallelProcessing, Int32(maxThreads))
        } else if let standardFunc = c_calculateFolderSizes {
            resultPtr = standardFunc(cPath)
        } else {
            print("C++ functions not available")
            return nil
        }
        
        // Process the result pointer
        guard let ptr = resultPtr else {
            return nil
        }
        
        defer {
            // Release the result pointer when done
            c_releaseResult?(ptr)
        }
        
        guard let getResultRootNodeFunc = c_getResultRootNode,
              let getResultElapsedTimeMsFunc = c_getResultElapsedTimeMs else {
            print("C++ accessor functions not available")
            return nil
        }
        
        let elapsedTimeMs = getResultElapsedTimeMsFunc(ptr)
        
        if let nodePtr = getResultRootNodeFunc(ptr) {
            let rootNode = FileNode(nodePtr: nodePtr)
            return FolderSizeResult(rootNode: rootNode, elapsedTimeMs: elapsedTimeMs)
        }
        
        return nil
    }
}

// Example usage in a macOS app:
/*
import Cocoa

class ViewController: NSViewController {
    // Create calculator with parallel processing (using all available cores)
    let calculator = FolderSizeCalculator(useParallelProcessing: true, maxThreads: 0)
    
    @IBAction func selectFolder(_ sender: Any) {
        let openPanel = NSOpenPanel()
        openPanel.canChooseDirectories = true
        openPanel.canChooseFiles = false
        openPanel.allowsMultipleSelection = false
        
        openPanel.begin { [weak self] (result) in
            if result == .OK, let url = openPanel.url {
                self?.processFolder(at: url.path)
            }
        }
    }
    
    func processFolder(at path: String) {
        // Run in background to avoid blocking UI
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            if let result = self?.calculator.calculateFolderSizes(at: path) {
                // Update UI on main thread
                DispatchQueue.main.async {
                    self?.displayResults(result)
                }
            }
        }
    }
    
    func displayResults(_ result: FolderSizeResult) {
        // Here you would update your UI with the results
        print("Total size: \(ByteCountFormatter.string(fromByteCount: Int64(result.rootNode.size), countStyle: .file))")
        print("Time taken: \(String(format: "%.2f", result.elapsedTimeMs)) ms")
        
        // Example: populate an outline view with the tree data
        // outlineView.reloadData()
    }
}
*/ 