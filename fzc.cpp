#include "fzc.hpp"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>
#include <limits.h>
#include <sstream>
#include <future>
#include <mutex>
#include <unordered_set>

namespace fs = std::filesystem;

uint64_t getAllocatedSize(const std::string& path) {
    char buf[sizeof(uint32_t) + sizeof(uint64_t)] = {0};
    struct attrlist attrList = {};
    attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrList.fileattr = ATTR_FILE_ALLOCSIZE;
    *reinterpret_cast<uint32_t*>(buf) = sizeof(buf);
    if (getattrlist(path.c_str(), &attrList, buf, sizeof(buf), 0) != 0) {
        std::cerr << "getattrlist failed on " << path << ": " << strerror(errno) << "\n";
        return 0;
    }
    uint64_t allocsize = *reinterpret_cast<uint64_t*>(buf + sizeof(uint32_t));
    return allocsize;
}

// Helper function declarations
inline bool startsWith(const std::string& str, const std::string& prefix) {
    return str.length() >= prefix.length() && 
           str.substr(0, prefix.length()) == prefix;
}

// Add this function before FZC methods
static std::string normalizePath(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    // Remove trailing slashes except root
    while (p.length() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool FZC::isSymLink(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }
    return (st.st_mode & S_IFMT) == S_IFLNK;
}

// 修改 getFileInfo 使 symlink 返回其自身大小
std::pair<uint64_t, bool> FZC::getFileInfo(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return {0, false};
    }
    if ((st.st_mode & S_IFMT) == S_IFLNK) {
        // symlink: size 为链接本身长度
        std::cout << "[symlink size] " << path << " size=" << st.st_size << std::endl;
        return {static_cast<uint64_t>(st.st_size), false};
    }
    bool isDir = false;
    try {
        isDir = fs::is_directory(path);
    } catch (...) {
        return {0, false};
    }
    uint64_t size = getAllocatedSize(path);
    return {size, isDir};
}

FZC::FZC(bool useParallelProcessing, int maxThreads)
    : m_maxThreads(maxThreads > 0 ? maxThreads : std::thread::hardware_concurrency()),
      m_maxDepthForParallelism(8) {
    if (m_maxThreads < 1) m_maxThreads = 1;
    m_mountPoints = getMountPoints();
    // firmlink 映射表初始化
    m_firmlinkMap = {
        {"/AppleInternal", "AppleInternal"},
        {"/Applications", "Applications"},
        {"/Library", "Library"},
        {"/System/Library/Caches", "System/Library/Caches"},
        {"/System/Library/Assets", "System/Library/Assets"},
        {"/System/Library/PreinstalledAssets", "System/Library/PreinstalledAssets"},
        {"/System/Library/AssetsV2", "System/Library/AssetsV2"},
        {"/System/Library/PreinstalledAssetsV2", "System/Library/PreinstalledAssetsV2"},
        {"/System/Library/CoreServices/CoreTypes.bundle/Contents/Library", "System/Library/CoreTypes.bundle/Contents/Library"},
        {"/System/Library/Speech", "System/Library/Speech"},
        {"/Users", "Users"},
        {"/Volumes", "Volumes"},
        {"/cores", "cores"},
        {"/opt", "opt"},
        {"/private", "private"},
        {"/usr/local", "usr/local"},
        {"/usr/libexec/cups", "usr/libexec/cups"},
        {"/usr/share/snmp", "usr/share/snmp"}
    };
    // 原始系统盘根路径，可根据实际情况扩展
    m_dataRoots = {
        "/System/Volumes/Data",
        // 你可以在这里添加其它原始系统盘根路径，比如 "/Volumes/Macintosh HD"
    };
}

bool is_hard_link(const std::string& path1, const std::string& path2) {
    struct stat st1, st2;
    if (lstat(path1.c_str(), &st1) != 0 || lstat(path2.c_str(), &st2) != 0) {
        perror("stat failed");
        return false;
    }
    return (st1.st_ino == st2.st_ino);
}

std::unordered_set<std::string> FZC::getMountPoints() {
    std::unordered_set<std::string> mountPoints;
    struct statfs* mntbuf;
    int mounts = getmntinfo(&mntbuf, MNT_WAIT);
    if (mounts > 0) {
        for (int i = 0; i < mounts; i++) {
            const auto& fs = mntbuf[i];
            std::string mountPath = fs.f_mntonname;
            if (strcmp(fs.f_mntonname, "/") != 0) {
                if ((fs.f_flags & MNT_LOCAL) == 0 ||
                    (fs.f_flags & MNT_REMOVABLE) ||
                    strncmp(fs.f_fstypename, "apfs", 4)) {
                    mountPoints.insert(mountPath);
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

FolderSizeResult FZC::calculateFolderSizes(const std::string& path, bool rootOnly, bool includeDirectorySize) {
    auto startTime = std::chrono::high_resolution_clock::now();
    fs::path fsPath(path);
    std::shared_ptr<FileNode> rootNode;
    try {
        if (isSymLink(path)) {
            rootNode = processFile(path);
        } else if (fs::is_regular_file(fsPath)) {
            rootNode = processFile(path);
        } else if (fs::is_directory(fsPath)) {
            rootNode = processDirectoryParallel(path, 0, rootOnly, includeDirectorySize);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error processing path: " << e.what() << std::endl;
        rootNode = nullptr;
    }
    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return FolderSizeResult(rootNode, elapsedTimeMs);
}

bool FZC::shouldSkipDirectory(const std::string& path) {
    if (isCoveredByFirmlink(path)) return true;
    if (m_processedPaths.empty()) {
        m_entryPath = path;
    }
    if (isMountPoint(path)) {
        if (path == m_entryPath) return false;
        if (startsWith(path, m_entryPath + "/")) return true;
        return false;
    }
    if (isSubPathOfMountPoint(path)) {
        if (startsWith(path, m_entryPath + "/") && isMountPoint(m_entryPath)) return false;
        return true;
    }
    return false;
}

bool FZC::hasAccessPermission(const std::string& path) {
    return access(path.c_str(), R_OK) == 0;
}

std::shared_ptr<FileNode> FZC::processDirectoryParallel(const std::string& path, int depth, bool rootOnly, bool includeDirectorySize) {
    try {
        fs::path dirPath(path);
        std::string workPath = dirPath.string();
        auto node = std::make_shared<FileNode>(workPath, workPath, 0, true);
        if (!hasAccessPermission(workPath)) return node;
        if (isSymLink(path)) return processFile(path);
        fs::path parentPath = dirPath.parent_path();
        if (parentPath != "/" && dirPath.has_parent_path()) {
            std::string rootSubPath = "/" + dirPath.filename().string();
            if (fs::exists(rootSubPath) && is_hard_link(workPath, rootSubPath)) return nullptr;
        }
        if (!fs::exists(dirPath)) return node;
        if (shouldSkipDirectory(path)) return node;
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            if (m_processedPaths.find(workPath) != m_processedPaths.end()) return nullptr;
            m_processedPaths.insert(workPath);
        }
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_pathMap[workPath] = workPath;
        }
        std::vector<fs::directory_entry> batch;
        batch.reserve(BATCH_SIZE);
        std::vector<std::future<std::shared_ptr<FileNode>>> futures;
        try {
            for (const auto& entry : fs::directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                try {
                    batch.push_back(entry);
                    if (batch.size() >= BATCH_SIZE) {
                        processBatch(batch, node, depth, futures, includeDirectorySize);
                    }
                } catch (const fs::filesystem_error&) {
                    continue;
                }
            }
            if (!batch.empty()) {
                processBatch(batch, node, depth, futures, includeDirectorySize);
            }
            for (auto& future : futures) {
                try {
                    if (auto childNode = future.get()) {
                        node->size += childNode->size;
                        node->children.push_back(childNode);
                    }
                } catch (const std::exception&) {}
            }
            if (!node->children.empty()) {
                std::sort(node->children.begin(), node->children.end(),
                          [](const auto& a, const auto& b) {
                              if (a->size != b->size) return a->size > b->size;
                              return a->path < b->path;
                          });
            }
            if (rootOnly) node->children.clear();
        } catch (const std::exception& e) {
            std::cerr << "Error processing directory: " << e.what() << std::endl;
            return node;
        }
        return node;
    } catch (const std::exception& e) {
        std::cerr << "Error processing directory: " << e.what() << std::endl;
        return std::make_shared<FileNode>(path, path, 0, true);
    }
}

void FZC::processBatch(
    std::vector<fs::directory_entry>& batch,
    std::shared_ptr<FileNode>& node,
    int depth,
    std::vector<std::future<std::shared_ptr<FileNode>>>& futures,
    bool includeDirectorySize) {
    for (const auto& entry : batch) {
        try {
            std::string workPath = entry.path().string();
            if (!hasAccessPermission(workPath)) {
                auto unauthorizedNode = std::make_shared<FileNode>(workPath, workPath, 0, false);
                node->children.push_back(unauthorizedNode);
                continue;
            }
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
                                                 [this, workPath, depth, includeDirectorySize]() {
                                                     auto result = processDirectoryParallel(workPath, depth + 1, false, includeDirectorySize);
                                                     m_activeThreads--;
                                                     return result;
                                                 }));
                    continue;
                }
            }
            if (isDir) {
                auto childNode = processDirectoryParallel(workPath, depth + 1, false, includeDirectorySize);
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
        // ...existing code...
        if (size == 0 && !isDir) {
            // 返回节点，size=0，isDir=false，保证结构可见
            std::string workPath = fs::path(path).string();
            {
                std::lock_guard<std::mutex> lock(m_pathMapMutex);
                m_pathMap[workPath] = workPath;
            }
            return std::make_shared<FileNode>(workPath, workPath, 0, false);
        }
        std::string workPath = fs::path(path).string();
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = workPath;
        }
        return std::make_shared<FileNode>(workPath, workPath, size, isDir);
    } catch (const std::exception&) {
        // 返回节点，size=0，isDir=false，保证结构可见
        std::string workPath = fs::path(path).string();
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = workPath;
        }
        return std::make_shared<FileNode>(workPath, workPath, 0, false);
    }
}

bool FZC::isCoveredByFirmlink(const std::string& path) {
    std::string normPath = normalizePath(path);
    for (const auto& root : m_dataRoots) {
        std::string normRoot = normalizePath(root);
        if (normPath == normRoot || !startsWith(normPath, normRoot + "/")) continue;
        // 取相对路径，带前导斜杠
        std::string rel = normPath.substr(normRoot.length());
        if (rel.empty()) rel = "/";
        if (rel[0] != '/') rel = "/" + rel;
        for (const auto& kv : m_firmlinkMap) {
            std::string value = kv.second;
            if (value.empty() || value[0] != '/') value = "/" + value;
            // 完全匹配或子路径匹配
            if (rel == value || startsWith(rel, value + "/")) {
                std::cout << "[firmlink skip] " << path << std::endl;
                return true;
            }
        }
    }
    return false;
}

// C-style interface implementation
extern "C" {
    FolderSizeResultPtr calculateFolderSizes(const char* rootPath, bool rootOnly, bool includeDirectorySize) {
        try {
            FZC calculator(true, 0);
            auto result = calculator.calculateFolderSizes(rootPath, rootOnly, includeDirectorySize);
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