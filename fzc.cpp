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

// Helper function to convert path to UTF-8 string and handle long paths
std::pair<std::string, std::string> FZC::toUTF8AndWorkPath(const fs::path& path) {
    std::string fullPath = path.string();  // std::filesystem already handles UTF-8 on macOS
    std::string workPath = fullPath;
    return {fullPath, workPath};
}

/// Use bit mode to check if path is a symlink.
bool FZC::isSymLink(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }
    return (st.st_mode & 0170000) == 0120000;
}

std::pair<uint64_t, bool> FZC::getFileInfo(const std::string& path, bool followSymlink) {
    struct stat st;
    int result;
    
    if (followSymlink) {
        result = stat(path.c_str(), &st);
    } else {
        result = lstat(path.c_str(), &st);
    }
    
    if (result != 0) {
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

bool FZC::canAccessDirectory(const std::string& path) {
    try {
        // 尝试打开目录
        fs::directory_iterator it(path);
        return true;
    } catch (const fs::filesystem_error& e) {
        return false;
    }
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
        
        if (!canAccessDirectory(path)) {
            auto pathPair = toUTF8AndWorkPath(dirPath);
            auto node = std::make_shared<FileNode>(pathPair.first, pathPair.second, 0, true);
            auto [size, _] = getFileInfo(path, true);
            node->size = size;
            return node;
        }
        
        auto pathPair = toUTF8AndWorkPath(dirPath);
        std::string fullPath = pathPair.first;
        std::string workPath = pathPair.second;
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_processedPaths.find(workPath) != m_processedPaths.end()) {
                return nullptr;
            }
            m_processedPaths.insert(workPath);
        }
        
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = fullPath;
        }
        
        // Create node and get the directory entry size
        auto node = std::make_shared<FileNode>(fullPath, workPath, 0, true);
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
            for (const auto& entry : fs::directory_iterator(dirPath)) {
                try {
                    batch.push_back(entry);
                    if (batch.size() >= BATCH_SIZE) {
                        processBatch(batch, node, depth, futures);
                    }
                } catch (const fs::filesystem_error&) {
                    // 跳过无法访问的条目
                    continue;
                }
            }
            
            if (!batch.empty()) {
                processBatch(batch, node, depth, futures);
            }
        } catch (const fs::filesystem_error&) {
            // 如果遍历过程中出现权限错误，返回已收集的信息
            return node;
        }
        
        // Process futures if any
        for (auto& future : futures) {
            try {
                if (auto childNode = future.get()) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
            } catch (const std::exception&) {
                // Silently handle future errors
            }
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
    std::vector<std::pair<std::string, int>> parallelDirs;
    
    for (const auto& entry : batch) {
        try {
            auto pathPair = toUTF8AndWorkPath(entry.path());
            std::string fullPath = pathPair.first;
            std::string workPath = pathPair.second;
            
            if (isSymLink(workPath)) {
                auto [size, _] = getFileInfo(workPath, false);
                auto symlinkNode = std::make_shared<FileNode>(fullPath, workPath, size, false);
                node->size += symlinkNode->size;
                node->children.push_back(symlinkNode);
                continue;
            }
            
            auto [size, isDir] = getFileInfo(workPath, true);
            if (isDir) {
                if (!canAccessDirectory(workPath)) {
                    auto dirNode = std::make_shared<FileNode>(fullPath, workPath, size, true);
                    node->size += size;
                    node->children.push_back(dirNode);
                    continue;
                }
                
                bool useParallel = (depth < m_maxDepthForParallelism && m_useParallelProcessing);
                if (useParallel) {
                    parallelDirs.emplace_back(workPath, depth);
                    continue;
                }
                
                // Process sequentially if we can't use parallelism
                auto childNode = processDirectory(workPath, depth + 1, false);
                if (childNode) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
            } else {
                if (size > 0) {
                    auto fileNode = std::make_shared<FileNode>(fullPath, workPath, size, false);
                    node->size += size;
                    node->children.push_back(fileNode);
                }
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    
    if (!parallelDirs.empty()) {
        std::unique_lock<std::mutex> lock(m_threadMutex);
        for (const auto& dirInfo : parallelDirs) {
            const std::string& workPath = dirInfo.first;
            const int dirDepth = dirInfo.second;
            
            if (m_activeThreads < m_maxThreads) {
                m_activeThreads++;
                lock.unlock();
                
                auto future = std::async(std::launch::async, [this, workPath, dirDepth]() {
                    auto result = processDirectoryParallel(workPath, dirDepth + 1, false);
                    m_activeThreads--;
                    return result;
                });
                futures.push_back(std::move(future));
                
                lock.lock();
            } else {
                lock.unlock();
                auto childNode = processDirectory(workPath, dirDepth + 1, false);
                if (childNode) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
                lock.lock();
            }
        }
    }
    
    batch.clear();
}

std::shared_ptr<FileNode> FZC::processFile(const std::string& path) {
    try {
        auto [size, isDir] = getFileInfo(path, false);
        if (size == 0 && !isDir) {
            return nullptr;
        }
        
        auto pathPair = toUTF8AndWorkPath(fs::path(path));
        std::string fullPath = pathPair.first;
        std::string workPath = pathPair.second;
        
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = fullPath;
        }
        
        return std::make_shared<FileNode>(fullPath, workPath, size, isDir);
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