#include "fzc.hpp"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>
#include <limits.h>
#include <sstream>

namespace fs = std::filesystem;

// Helper function to ensure the /tmp/udu directory exists
void FZC::ensureTempDirExists() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        fs::path tempDir("/tmp/udu");
        if (!fs::exists(tempDir)) {
            fs::create_directory(tempDir);
        }
    });
}

// Helper function to get a shortened path and create a symbolic link
std::string FZC::getShortenedPath(const std::string& path) {
    if (path.length() < PATH_MAX) {
        return path;
    }
    
    ensureTempDirExists();
    
    // Create a hash of the path
    std::hash<std::string> hasher;
    size_t hash = hasher(path);
    
    // Get the filename or last component
    fs::path fsPath(path);
    std::string filename = fsPath.filename().string();
    
    // Create a shortened path with hash
    std::stringstream ss;
    ss << "/tmp/udu/" << std::hex << hash;
    if (!filename.empty()) {
        ss << "_" << filename;
    }
    std::string shortPath = ss.str();
    
    // Create symbolic link if it doesn't exist
    try {
        if (!fs::exists(shortPath)) {
            fs::create_symlink(fsPath, shortPath);
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to create symlink for " << path << ": " << e.what() << std::endl;
        return path; // Fall back to original path if symlink creation fails
    }
    
    return shortPath;
}

// Helper function to convert path to UTF-8 string and handle long paths
std::pair<std::string, std::string> FZC::toUTF8AndWorkPath(const fs::path& path) {
    std::string fullPath = path.string();  // std::filesystem already handles UTF-8 on macOS
    std::string workPath = getShortenedPath(fullPath);
    return {fullPath, workPath};
}

// Constructor with parallelism configuration
FZC::FZC(bool useParallelProcessing, int maxThreads)
    : m_useParallelProcessing(useParallelProcessing),
      m_maxThreads(maxThreads),
      m_maxDepthForParallelism(4) // Only use parallelism for top levels of directory tree
{
    // If maxThreads is 0 or negative, use the number of hardware threads
    if (m_maxThreads <= 0) {
        m_maxThreads = std::thread::hardware_concurrency();
        // Ensure at least 2 threads
        if (m_maxThreads < 2) m_maxThreads = 2;
    }
}

FolderSizeResult FZC::calculateFolderSizes(const std::string& path) {
    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Check if path is a file, directory, or symlink
    fs::path fsPath(path);
    std::shared_ptr<FileNode> rootNode;
    
    try {
        if (fs::is_symlink(fsPath)) {
            // For symlinks, just get the symlink size without following
            auto pathPair = toUTF8AndWorkPath(fsPath);
            struct stat st;
            if (lstat(pathPair.second.c_str(), &st) == 0) {
                rootNode = std::make_shared<FileNode>(pathPair.first, pathPair.second, st.st_size, false);
            } else {
                rootNode = std::make_shared<FileNode>(pathPair.first, pathPair.second, 0, false);
            }
        } else if (fs::is_regular_file(fsPath)) {
            rootNode = processFile(path);
        } else if (fs::is_directory(fsPath)) {
            if (m_useParallelProcessing) {
                rootNode = processDirectoryParallel(path);
            } else {
                rootNode = processDirectory(path);
            }
        } else {
            throw std::runtime_error("Path is neither a regular file nor a directory");
        }
    } catch (const std::exception&) {
        return FolderSizeResult(nullptr, 0.0);
    }
    
    // End timing
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Return result with timing information
    return FolderSizeResult(rootNode, static_cast<double>(duration.count()));
}

std::shared_ptr<FileNode> FZC::processDirectory(const std::string& path, int depth) {
    try {
        fs::path dirPath(path);
        if (!fs::exists(dirPath)) {
            return nullptr;
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
                batch.push_back(entry);
                if (batch.size() >= BATCH_SIZE) {
                    processBatch(batch, node, depth, futures);
                }
            }
            
            if (!batch.empty()) {
                processBatch(batch, node, depth, futures);
            }
        } catch (const std::exception&) {
            // Silently handle directory iteration errors
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
            std::sort(node->children.begin(), node->children.end(),
                     [](const auto& a, const auto& b) { return a->size > b->size; });
        }
        
        return node;
    } catch (const std::exception&) {
        // Return nullptr for any directory we can't access
        return nullptr;
    }
}

std::shared_ptr<FileNode> FZC::processDirectoryParallel(const std::string& path, int depth) {
    return processDirectory(path, depth);  // Use the same optimized implementation
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
            auto pathPair = toUTF8AndWorkPath(entry.path());
            std::string fullPath = pathPair.first;
            std::string workPath = pathPair.second;
            
            if (fs::is_symlink(entry)) {
                // For symlinks, just get the symlink size without following
                struct stat st;
                if (lstat(workPath.c_str(), &st) == 0) {
                    auto symlinkNode = std::make_shared<FileNode>(fullPath, workPath, st.st_size, false);
                    node->size += symlinkNode->size;
                    node->children.push_back(symlinkNode);
                }
            } else if (entry.is_directory()) {
                bool useParallel = (depth < m_maxDepthForParallelism && m_useParallelProcessing);
                if (useParallel) {
                    // Check if we have available threads
                    std::unique_lock<std::mutex> lock(m_threadMutex);
                    if (m_activeThreads < m_maxThreads) {
                        m_activeThreads++;
                        lock.unlock();
                        
                        // Store workPath in a separate variable to avoid capturing structured bindings
                        std::string capturedWorkPath = workPath;
                        // Process in parallel
                        auto future = std::async(std::launch::async, [this, capturedWorkPath, depth]() {
                            auto result = processDirectoryParallel(capturedWorkPath, depth + 1);
                            m_activeThreads--;
                            return result;
                        });
                        futures.push_back(std::move(future));
                        continue;
                    }
                }
                
                // Process sequentially if we can't use parallelism
                auto childNode = processDirectory(workPath, depth + 1);
                if (childNode) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
            } else if (entry.is_regular_file()) {
                uint64_t fileSize = getFileSize(workPath);
                if (fileSize > 0) {
                    auto fileNode = std::make_shared<FileNode>(fullPath, workPath, fileSize, false);
                    node->size += fileSize;
                    node->children.push_back(fileNode);
                }
            }
        } catch (const std::exception&) {
            // Silently skip entries we can't access
            continue;
        }
    }
    batch.clear();
}

std::shared_ptr<FileNode> FZC::processFile(const std::string& path) {
    try {
        fs::path filePath(path);
        if (!fs::exists(filePath)) {
            return nullptr;
        }
        
        auto pathPair = toUTF8AndWorkPath(filePath);
        std::string fullPath = pathPair.first;
        std::string workPath = pathPair.second;
        
        {
            std::lock_guard<std::mutex> lock(m_pathMapMutex);
            m_pathMap[workPath] = fullPath;
        }
        
        if (fs::is_symlink(filePath)) {
            // For symlinks, just get the symlink size without following
            struct stat st;
            if (lstat(workPath.c_str(), &st) == 0) {
                return std::make_shared<FileNode>(fullPath, workPath, st.st_size, false);
            }
            return std::make_shared<FileNode>(fullPath, workPath, 0, false);
        }
        
        uint64_t fileSize = getFileSize(workPath);
        if (fileSize > 0) {
            return std::make_shared<FileNode>(fullPath, workPath, fileSize, false);
        }
        return nullptr;
    } catch (const std::exception&) {
        // Return nullptr for any file we can't access
        return nullptr;
    }
}

// C-style interface implementation
extern "C" {
    FolderSizeResultPtr calculateFolderSizes(const char* rootPath) {
        try {
            // Use default settings (parallel processing with auto thread count)
            FZC calculator(true, 0);
            auto result = calculator.calculateFolderSizes(rootPath);
            // Transfer ownership to C interface
            return static_cast<void*>(new FolderSizeResult(std::move(result)));
        } catch (const std::exception& e) {
            std::cerr << "Error calculating folder sizes: " << e.what() << std::endl;
            return nullptr;
        }
    }
    
    FolderSizeResultPtr calculateFolderSizesParallel(const char* rootPath, bool useParallelProcessing, int maxThreads) {
        try {
            FZC calculator(useParallelProcessing, maxThreads);
            auto result = calculator.calculateFolderSizes(rootPath);
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