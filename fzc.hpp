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
#include <locale>
#include <codecvt>
#include <unordered_map>
#include <deque>
#include <condition_variable>

namespace fs = std::filesystem;

// Node structure to represent files and directories in the tree
struct FileNode {
    std::string path;      // Display path (can be full length)
    std::string workPath;  // Working path (shortened if needed)
    uint64_t size;
    bool isDirectory;
    std::vector<std::shared_ptr<FileNode>> children;
    
    FileNode(const std::string& p, const std::string& wp, uint64_t s, bool isDir) 
        : path(p), workPath(wp), size(s), isDirectory(isDir) {}
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
    FolderSizeResult calculateFolderSizes(const std::string& path, bool rootOnly = false, bool includeDirectorySize = true);
    
private:
    // Ensure temporary directory exists
    static void ensureTempDirExists();
    
    // Process a single file
    std::shared_ptr<FileNode> processFile(const std::string& path);
    
    // Recursive function to process directories
    std::shared_ptr<FileNode> processDirectory(const std::string& path, int depth = 0, bool rootOnly = false, bool includeDirectorySize = true);
    
    // Parallel version of directory processing
    std::shared_ptr<FileNode> processDirectoryParallel(const std::string& path, int depth = 0, bool rootOnly = false, bool includeDirectorySize = true);
    
    // Helper function to get file size
    uint64_t getFileSize(const std::string& path);
    
    // Helper function to process a batch of entries
    void processBatch(std::vector<fs::directory_entry>& batch,
                     std::shared_ptr<FileNode>& node,
                     int depth,
                     std::vector<std::future<std::shared_ptr<FileNode>>>& futures,
                     bool includeDirectorySize);
    
    // Configuration
    bool m_useParallelProcessing;
    int m_maxThreads;
    int m_maxDepthForParallelism;
    static constexpr size_t BATCH_SIZE = 64;
    
    // Thread management
    std::atomic<int> m_activeThreads{0};
    std::mutex m_threadMutex;
    
    // Cache for processed paths
    std::unordered_set<std::string> m_processedPaths;
    std::mutex m_cacheMutex;
    
    // Map to store working path to full path mapping
    std::unordered_map<std::string, std::string> m_pathMap;
    std::mutex m_pathMapMutex;

    bool isSymLink(const std::string& path);
    std::pair<uint64_t, bool> getFileInfo(const std::string& path);
    bool shouldSkipDirectory(const std::string& path);
    std::unordered_set<std::string> getMountPoints();
    bool isMountPoint(const std::string& path);
    bool isSubPathOfMountPoint(const std::string& path);
    std::unordered_set<std::string> m_mountPoints;
    std::string m_entryPath;  // 保存入口路径

    bool hasAccessPermission(const std::string& path);
    std::unordered_map<std::string, std::string> m_firmlinkMap; // key: installed system path, value: original system path
    std::vector<std::string> m_dataRoots; // 原始系统盘根路径
    bool isCoveredByFirmlink(const std::string& path);

    std::string m_entryFsType;
    uint64_t getFileSizeByFsType(const std::string& path);
};

// C-style interface for Swift interoperability
extern "C" {
    // Opaque pointer types
    typedef void* FileNodePtr;
    typedef void* FolderSizeResultPtr;
    
    // Function to calculate folder sizes and return the result
    FolderSizeResultPtr calculateFolderSizes(const char* rootPath, bool rootOnly, bool includeDirectorySize);
    
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