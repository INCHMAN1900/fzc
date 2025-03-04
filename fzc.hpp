#ifndef FZC_HPP
#define FZC_HPP

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <future>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <filesystem>

namespace fs = std::filesystem;

// Node structure to represent files and directories in the tree
struct FileNode {
    std::string path;
    uint64_t size;
    bool isDirectory;
    std::vector<std::shared_ptr<FileNode>> children;
    
    FileNode(const std::string& p, uint64_t s, bool isDir) 
        : path(p), size(s), isDirectory(isDir) {}
};

// Result structure that includes timing information
struct FolderSizeResult {
    std::shared_ptr<FileNode> rootNode;
    double elapsedTimeMs;
    
    FolderSizeResult(std::shared_ptr<FileNode> node, double timeMs)
        : rootNode(node), elapsedTimeMs(timeMs) {}
};

// Main class for calculating folder sizes
class FZC {
public:
    // Constructor with configurable parallelism
    FZC(bool useParallelProcessing = true, int maxThreads = 0);
    
    // Calculate sizes and return the root node with timing information
    FolderSizeResult calculateFolderSizes(const std::string& rootPath);
    
private:
    // Recursive function to process directories
    std::shared_ptr<FileNode> processDirectory(const std::string& path, int depth = 0);
    
    // Parallel version of directory processing
    std::shared_ptr<FileNode> processDirectoryParallel(const std::string& path, int depth = 0);
    
    // Helper function to get file size
    uint64_t getFileSize(const std::string& path);
    
    // Helper function to process a batch of entries
    void processBatch(std::vector<fs::directory_entry>& batch,
                     std::shared_ptr<FileNode>& node,
                     int depth,
                     std::vector<std::future<std::shared_ptr<FileNode>>>& futures);
    
    // Configuration
    bool m_useParallelProcessing;
    int m_maxThreads;
    int m_maxDepthForParallelism;
    static constexpr size_t BATCH_SIZE = 100;  // Size of batches for processing
    
    // Thread management
    std::atomic<int> m_activeThreads{0};
    std::mutex m_threadMutex;
    
    // Cache for processed paths to avoid cycles
    std::unordered_set<std::string> m_processedPaths;
    std::mutex m_cacheMutex;
};

// C-style interface for Swift interoperability
extern "C" {
    // Opaque pointer type for FileNode
    typedef void* FileNodePtr;
    
    // Opaque pointer type for FolderSizeResult
    typedef void* FolderSizeResultPtr;
    
    // Function to calculate folder sizes and return the result
    FolderSizeResultPtr calculateFolderSizes(const char* rootPath);
    
    // Function to calculate folder sizes with parallel processing options
    FolderSizeResultPtr calculateFolderSizesParallel(const char* rootPath, bool useParallelProcessing, int maxThreads);
    
    // Functions to access node properties
    const char* getNodePath(FileNodePtr node);
    uint64_t getNodeSize(FileNodePtr node);
    bool isNodeDirectory(FileNodePtr node);
    int getChildrenCount(FileNodePtr node);
    FileNodePtr getChildNode(FileNodePtr node, int index);
    
    // Functions to access result properties
    FileNodePtr getResultRootNode(FolderSizeResultPtr result);
    double getResultElapsedTimeMs(FolderSizeResultPtr result);
    
    // Functions to free memory
    void releaseFileNode(FileNodePtr node);
    void releaseResult(FolderSizeResultPtr result);
}

#endif // FZC_HPP 