#include "fzc.hpp"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>
#include <limits.h>
#include <sstream>

namespace fs = std::filesystem;

// Ignored paths.
const std::vector<std::string> FZC::skipPaths = {
    "/System/Volumes/Data",
    "/System/Volumes/Preboot",
    "/System/Volumes/VM",
    "/System/Volumes/Update",
    "/Volumes"  // 外接盘目录
};

/// Use bit mode to check if path is a symlink.
bool FZC::isSymLink(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }
    return (st.st_mode & 0170000) == 0120000;
}

std::pair<uint64_t, bool> FZC::getFileInfo(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return {0, false};
    }
    return {st.st_size, (st.st_mode & S_IFMT) == S_IFDIR};
}

// Constructor with parallelism configuration
FZC::FZC(bool useParallelProcessing, int maxThreads)
    : m_useParallelProcessing(useParallelProcessing),
      m_maxThreads(maxThreads),
      m_maxDepthForParallelism(8)
{
    int systemCores = std::thread::hardware_concurrency();
    if (m_maxThreads <= 0) {
        m_maxThreads = systemCores;
        if (m_maxThreads < 1) m_maxThreads = 1;
    }
}

FolderSizeResult FZC::calculateFolderSizes(const std::string& path, bool rootOnly) {
    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Check if path is a file, directory, or symlink
    fs::path fsPath(path);
    std::shared_ptr<FileNode> rootNode;
    
    try {
        if (isSymLink(path)) {
            // For symlinks, just get the symlink size without following
            rootNode = processFile(path);
        } else if (fs::is_regular_file(fsPath)) {
            rootNode = processFile(path);
        } else if (fs::is_directory(fsPath)) {
            if (m_useParallelProcessing) {
                rootNode = processDirectoryParallel(path, 0, rootOnly);
            } else {
                rootNode = processDirectory(path, 0, rootOnly);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error processing path: " << e.what() << std::endl;
        rootNode = nullptr;
    }

    // Calculate elapsed time
    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    return FolderSizeResult(rootNode, elapsedTimeMs);
}

bool FZC::shouldSkipDirectory(const std::string& path) {
    for (const auto& skipPath : skipPaths) {
        if (path == skipPath || 
            (path.length() > skipPath.length() && 
             path.substr(0, skipPath.length()) == skipPath && 
             path[skipPath.length()] == '/')) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<FileNode> FZC::processDirectory(const std::string& path, int depth, bool rootOnly) {
    try {
        fs::path dirPath(path);
        if (!fs::exists(dirPath) || isSymLink(path)) {
            return processFile(path);
        }
        
        if (shouldSkipDirectory(path)) {
            return nullptr;
        }
        
        std::string workPath = dirPath.string();
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_processedPaths.find(workPath) != m_processedPaths.end()) {
                return nullptr;
            }
            m_processedPaths.insert(workPath);
        }
        
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = workPath;
        }
        
        // Create node and get the directory entry size
        auto node = std::make_shared<FileNode>(workPath, workPath, 0, true);
        try {
            // Get the directory entry size using stat to get accurate size
            struct stat st;
            if (stat(workPath.c_str(), &st) == 0) {
                node->size = st.st_size;
            }
        } catch (const std::exception&) {
            // If we can't get the directory size, just use 0
            node->size = 0;
        }
        
        std::vector<fs::directory_entry> batch;
        batch.reserve(BATCH_SIZE);
        std::vector<std::future<std::shared_ptr<FileNode>>> futures;
        
        try {
            for (const auto& entry : fs::directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                try {
                    batch.push_back(entry);
                    if (batch.size() >= BATCH_SIZE) {
                        processBatch(batch, node, depth, futures);
                    }
                } catch (const fs::filesystem_error&) {
                    continue;
                }
            }
            
            if (!batch.empty()) {
                processBatch(batch, node, depth, futures);
            }
            
            // 等待所有任务完成
            for (auto& future : futures) {
                try {
                    if (auto childNode = future.get()) {
                        node->size += childNode->size;
                        node->children.push_back(childNode);
                    }
                } catch (const std::exception&) {}
            }
            
        } catch (const fs::filesystem_error&) {
            return node;
        }
        
        if (!node->children.empty()) {
            // Sort by size in descending order, then by path for stable ordering
            std::sort(node->children.begin(), node->children.end(),
                     [](const auto& a, const auto& b) {
                         if (a->size != b->size) {
                             return a->size > b->size;  // Descending order by size
                         }
                         return a->path < b->path;      // Ascending order by path if sizes are equal
                     });
        }
        
        // If rootOnly is true, clear children after calculating total size
        if (rootOnly) {
            node->children.clear();
        }
        
        return node;
    } catch (const std::exception& e) {
        std::cerr << "Error processing directory: " << e.what() << std::endl;
        return nullptr;
    }
}

std::shared_ptr<FileNode> FZC::processDirectoryParallel(const std::string& path, int depth, bool rootOnly) {
    return processDirectory(path, depth, rootOnly);  // Use the same optimized implementation
}

uint64_t FZC::getFileSize(const std::string& path) {
    try {
        fs::path filePath(path);
        return fs::file_size(filePath);
    } catch (const std::exception&) {
        // Silently return 0 for any errors
        return 0;
    }
}

void FZC::processBatch(
    std::vector<fs::directory_entry>& batch,
    std::shared_ptr<FileNode>& node,
    int depth,
    std::vector<std::future<std::shared_ptr<FileNode>>>& futures)
{
    for (const auto& entry : batch) {
        try {
            std::string workPath = entry.path().string();
            
            if (isSymLink(workPath)) {
                auto [size, _] = getFileInfo(workPath);
                auto symlinkNode = std::make_shared<FileNode>(workPath, workPath, size, false);
                node->size += symlinkNode->size;
                node->children.push_back(symlinkNode);
                continue;
            }
            
            auto [size, isDir] = getFileInfo(workPath);
            if (isDir && depth < m_maxDepthForParallelism && m_activeThreads < m_maxThreads) {
                std::lock_guard<std::mutex> lock(m_threadMutex);
                if (m_activeThreads < m_maxThreads) {
                    m_activeThreads++;
                    futures.push_back(std::async(std::launch::async,
                        [this, workPath, depth]() {
                            auto result = processDirectory(workPath, depth + 1, false);
                            m_activeThreads--;
                            return result;
                        }));
                    continue;
                }
            }
            
            if (isDir) {
                auto childNode = processDirectory(workPath, depth + 1, false);
                if (childNode) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
            } else if (size > 0) {
                auto fileNode = std::make_shared<FileNode>(workPath, workPath, size, false);
                node->size += size;
                node->children.push_back(fileNode);
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    batch.clear();
}

std::shared_ptr<FileNode> FZC::processFile(const std::string& path) {
    try {
        auto [size, isDir] = getFileInfo(path);
        if (size == 0 && !isDir) {
            return nullptr;
        }
        
        std::string workPath = fs::path(path).string();
        
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = workPath;
        }
        
        return std::make_shared<FileNode>(workPath, workPath, size, isDir);
    } catch (const std::exception&) {
        return nullptr;
    }
}

// C-style interface implementation
extern "C" {
    FolderSizeResultPtr calculateFolderSizes(const char* rootPath, bool rootOnly) {
        try {
            // Use default settings (parallel processing with auto thread count)
            FZC calculator(true, 0);
            auto result = calculator.calculateFolderSizes(rootPath, rootOnly);
            // Transfer ownership to C interface
            return static_cast<void*>(new FolderSizeResult(std::move(result)));
        } catch (const std::exception& e) {
            std::cerr << "Error calculating folder sizes: " << e.what() << std::endl;
            return nullptr;
        }
    }
    
    FolderSizeResultPtr calculateFolderSizesParallel(const char* rootPath, bool useParallelProcessing, int maxThreads, bool rootOnly) {
        try {
            FZC calculator(useParallelProcessing, maxThreads);
            auto result = calculator.calculateFolderSizes(rootPath, rootOnly);
            // Transfer ownership to C interface
            return static_cast<void*>(new FolderSizeResult(std::move(result)));
        } catch (const std::exception& e) {
            std::cerr << "Error calculating folder sizes: " << e.what() << std::endl;
            return nullptr;
        }
    }
    
    FileNodePtr getResultRootNode(FolderSizeResultPtr result) {
        if (!result) return nullptr;
        auto folderResult = static_cast<FolderSizeResult*>(result);
        return static_cast<void*>(new std::shared_ptr<FileNode>(folderResult->rootNode));
    }
    
    double getResultElapsedTimeMs(FolderSizeResultPtr result) {
        if (!result) return 0.0;
        auto folderResult = static_cast<FolderSizeResult*>(result);
        return folderResult->elapsedTimeMs;
    }
    
    const char* getNodePath(FileNodePtr node) {
        if (!node) return nullptr;
        auto fileNode = *static_cast<std::shared_ptr<FileNode>*>(node);
        return fileNode->path.c_str();
    }
    
    uint64_t getNodeSize(FileNodePtr node) {
        if (!node) return 0;
        auto fileNode = *static_cast<std::shared_ptr<FileNode>*>(node);
        return fileNode->size;
    }
    
    bool isNodeDirectory(FileNodePtr node) {
        if (!node) return false;
        auto fileNode = *static_cast<std::shared_ptr<FileNode>*>(node);
        return fileNode->isDirectory;
    }
    
    int getChildrenCount(FileNodePtr node) {
        if (!node) return 0;
        auto fileNode = *static_cast<std::shared_ptr<FileNode>*>(node);
        return static_cast<int>(fileNode->children.size());
    }
    
    FileNodePtr getChildNode(FileNodePtr node, int index) {
        if (!node) return nullptr;
        auto fileNode = *static_cast<std::shared_ptr<FileNode>*>(node);
        if (index < 0 || index >= fileNode->children.size()) {
            return nullptr;
        }
        return static_cast<void*>(new std::shared_ptr<FileNode>(fileNode->children[index]));
    }
    
    void releaseFileNode(FileNodePtr node) {
        if (node) {
            delete static_cast<std::shared_ptr<FileNode>*>(node);
        }
    }
    
    void releaseResult(FolderSizeResultPtr result) {
        if (result) {
            delete static_cast<FolderSizeResult*>(result);
        }
    }
}