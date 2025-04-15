#include "fzc.hpp"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/mount.h>  // for getmntinfo
#include <sys/param.h>
#include <limits.h>
#include <sstream>

namespace fs = std::filesystem;

// Helper function declarations
inline bool startsWith(const std::string& str, const std::string& prefix) {
    return str.length() >= prefix.length() && 
           str.substr(0, prefix.length()) == prefix;
}

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
      m_maxDepthForParallelism(8) {
    int systemCores = std::thread::hardware_concurrency();
    if (m_maxThreads <= 0) {
        m_maxThreads = systemCores;
        if (m_maxThreads < 1) m_maxThreads = 1;
    }
    m_mountPoints = getMountPoints();
}

std::unordered_set<std::string> FZC::getMountPoints() {
    std::unordered_set<std::string> mountPoints;
    struct statfs* mntbuf;
    int mounts = getmntinfo(&mntbuf, MNT_WAIT);
    
    if (mounts > 0) {
        for (int i = 0; i < mounts; i++) {
            const auto& fs = mntbuf[i];
            std::string mountPath = fs.f_mntonname;
            
            // 排除系统盘
            if (strcmp(fs.f_mntonname, "/") != 0) {
                // 检查是否是外部文件系统
                if ((fs.f_flags & MNT_LOCAL) == 0 ||    // 非本地地盘
                    (fs.f_flags & MNT_REMOVABLE)) {     // 外接磁盘
                    mountPoints.insert(mountPath);
                    std::cout << "Mount point: " << mountPath << ", removable:" << (fs.f_flags & MNT_REMOVABLE) << ", apfs:" << strncmp(fs.f_fstypename, "apfs", 4) << std::endl;
                }
            }
        }
    }
    return mountPoints;
}

bool FZC::isMountPoint(const std::string& path) {
    return m_mountPoints.find(path) != m_mountPoints.end();
}

bool FZC::isSubPathOfMountPoint(const std::string& path) {
    if (path.empty()) return false;
    
    for (const auto& mount : m_mountPoints) {
        if (path.length() > mount.length() && 
            path.substr(0, mount.length()) == mount && 
            path[mount.length()] == '/') {
            return true;
        }
    }
    return false;
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
    // 首次处理时保存入口路径
    if (m_processedPaths.empty()) {
        m_entryPath = path;
    }
    
    // 如果是挂载点
    if (isMountPoint(path)) {
        // 如果是入口路径，不跳过
        if (path == m_entryPath) {
            return false;
        }
        // 如果不是入口路径，而且当前路径在入口路径下，则跳过
        if (startsWith(path, m_entryPath + "/")) {
            return true;
        }
        // 其他情况不跳过
        return false;
    }
    
    // 如果路径不是挂载点，但是其父目录中包含挂载点
    if (isSubPathOfMountPoint(path)) {
        // 如果在入口路径下，且入口路径是挂载点，则允许处理
        if (startsWith(path, m_entryPath + "/") && isMountPoint(m_entryPath)) {
            return false;
        }
        // 其他情况跳过
        return true;
    }
    
    return false;
}

std::shared_ptr<FileNode> FZC::processDirectory(const std::string& path, int depth, bool rootOnly) {
    try {
        fs::path dirPath(path);
        std::string workPath = dirPath.string();
        
        // 创建节点，初始大小为0
        auto node = std::make_shared<FileNode>(workPath, workPath, 0, true);
        
        // 如果是符号链接，作为文件处理
        if (isSymLink(path)) {
            return processFile(path);
        }
        
        // 如果目录不存在，返回空节点
        if (!fs::exists(dirPath)) {
            return node;
        }
        
        // 如果目录需要跳过，返回空节点
        if (shouldSkipDirectory(path)) {
            return node;
        }
        
        // 检查是否已处理过
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            if (m_processedPaths.find(workPath) != m_processedPaths.end()) {
                return nullptr;
            }
            m_processedPaths.insert(workPath);
        }
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_pathMap[workPath] = workPath;
        }
        
        // 获取目录本身的大小
        try {
            struct stat st;
            if (stat(workPath.c_str(), &st) == 0) {
                node->size = st.st_size;
            }
        } catch (const std::exception&) {
            // 保持 size 为 0
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
                } catch (const std::exception&) {
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
        } catch (const std::exception& e) {
            std::cerr << "Error processing directory: " << e.what() << std::endl;
            return node;
        }
        
        return node;
    } catch (const std::exception& e) {
        // 发生错误时返回空节点而不是 nullptr
        std::cerr << "Error processing directory: " << e.what() << std::endl;
        return std::make_shared<FileNode>(path, path, 0, true);
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
    std::vector<std::future<std::shared_ptr<FileNode>>>& futures) {
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
        void releaseResult(FolderSizeResultPtr result) {        if (result) {            delete static_cast<FolderSizeResult*>(result);        }    }
}