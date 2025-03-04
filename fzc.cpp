#include "fzc.hpp"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>

namespace fs = std::filesystem;

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

FolderSizeResult FZC::calculateFolderSizes(const std::string& rootPath) {
    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Process the directory (using parallel or sequential method based on configuration)
    std::shared_ptr<FileNode> rootNode;
    if (m_useParallelProcessing) {
        rootNode = processDirectoryParallel(rootPath);
    } else {
        rootNode = processDirectory(rootPath);
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
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_processedPaths.find(path) != m_processedPaths.end()) {
                return nullptr;
            }
            m_processedPaths.insert(path);
        }
        
        // Create node and get the directory entry size
        auto node = std::make_shared<FileNode>(path, 0, true);
        try {
            // Get the directory entry size using stat to get accurate size
            struct stat st;
            if (stat(path.c_str(), &st) == 0) {
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
            // Skip inaccessible directories
        }
        
        // Process futures if any
        for (auto& future : futures) {
            try {
                if (auto childNode = future.get()) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
            } catch (const std::exception&) {
                // Skip failed futures
            }
        }
        
        if (!node->children.empty()) {
            std::sort(node->children.begin(), node->children.end(),
                     [](const auto& a, const auto& b) { return a->size > b->size; });
        }
        
        return node;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::shared_ptr<FileNode> FZC::processDirectoryParallel(const std::string& path, int depth) {
    return processDirectory(path, depth);  // Use the same optimized implementation
}

uint64_t FZC::getFileSize(const std::string& path) {
    try {
        return fs::file_size(fs::path(path));
    } catch (const std::exception& e) {
        std::cerr << "Error getting file size for " << path 
                  << ": " << e.what() << std::endl;
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
            if (entry.is_directory()) {
                bool useParallel = (depth < m_maxDepthForParallelism && m_useParallelProcessing);
                if (useParallel) {
                    // Check if we have available threads
                    std::unique_lock<std::mutex> lock(m_threadMutex);
                    if (m_activeThreads < m_maxThreads) {
                        m_activeThreads++;
                        lock.unlock();
                        
                        // Process in parallel
                        auto future = std::async(std::launch::async, [this, entry, depth]() {
                            auto result = processDirectoryParallel(entry.path().string(), depth + 1);
                            m_activeThreads--;
                            return result;
                        });
                        futures.push_back(std::move(future));
                        continue;
                    }
                }
                
                // Process sequentially if we can't use parallelism
                auto childNode = processDirectory(entry.path().string(), depth + 1);
                if (childNode) {
                    node->size += childNode->size;
                    node->children.push_back(childNode);
                }
            } else if (entry.is_regular_file()) {
                uint64_t fileSize = getFileSize(entry.path().string());
                if (fileSize > 0) {
                    auto fileNode = std::make_shared<FileNode>(entry.path().string(), fileSize, false);
                    node->size += fileSize;
                    node->children.push_back(fileNode);
                }
            }
        } catch (const std::exception&) {
            // Skip problematic entries
        }
    }
    batch.clear();
}

// C-style interface implementation
extern "C" {
    FolderSizeResultPtr calculateFolderSizes(const char* rootPath) {
        // Use default settings (parallel processing with auto thread count)
        FZC calculator(true, 0);
        auto result = calculator.calculateFolderSizes(rootPath);
        // Transfer ownership to C interface
        return static_cast<void*>(new FolderSizeResult(std::move(result)));
    }
    
    FolderSizeResultPtr calculateFolderSizesParallel(const char* rootPath, bool useParallelProcessing, int maxThreads) {
        FZC calculator(useParallelProcessing, maxThreads);
        auto result = calculator.calculateFolderSizes(rootPath);
        // Transfer ownership to C interface
        return static_cast<void*>(new FolderSizeResult(std::move(result)));
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