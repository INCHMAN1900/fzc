#include "folder_size_calculator.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <cstring>

// Helper function to format file size
std::string formatSize(uint64_t size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double formattedSize = static_cast<double>(size);
    
    while (formattedSize >= 1024.0 && unitIndex < 4) {
        formattedSize /= 1024.0;
        unitIndex++;
    }
    
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << formattedSize << " " << units[unitIndex];
    return stream.str();
}

// Helper function to print the tree
void printTree(const std::shared_ptr<FileNode>& node, int level = 0) {
    if (!node) return;
    
    // Print indentation
    std::string indent(level * 2, ' ');
    
    // Print node info
    std::cout << indent << node->path << " (" 
              << (node->isDirectory ? "dir" : "file") << ", " 
              << formatSize(node->size) << ")" << std::endl;
    
    // Print children
    for (const auto& child : node->children) {
        printTree(child, level + 1);
    }
}

// Print usage information
void printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " [options] <directory_path>" << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  -t, --time-only       Display only the time taken for calculation" << std::endl;
    std::cerr << "  -s, --sequential      Use sequential processing (no parallelism)" << std::endl;
    std::cerr << "  -j, --threads <num>   Specify maximum number of threads to use" << std::endl;
    std::cerr << "  -h, --help            Display this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    // Parse command line arguments
    bool timeOnly = false;
    bool useParallelProcessing = true; // Default to parallel processing
    int maxThreads = 0; // Default to auto-detect
    std::string rootPath;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--time-only") == 0) {
            timeOnly = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sequential") == 0) {
            useParallelProcessing = false;
        } else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i + 1 < argc) {
                maxThreads = std::atoi(argv[++i]);
                if (maxThreads <= 0) {
                    std::cerr << "Warning: Invalid thread count. Using auto-detection." << std::endl;
                    maxThreads = 0;
                }
            } else {
                std::cerr << "Error: Missing thread count after -j/--threads option." << std::endl;
                printUsage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            // Assume this is the directory path
            rootPath = argv[i];
        }
    }
    
    if (rootPath.empty()) {
        std::cerr << "Error: No directory path specified." << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    // Create calculator with specified options
    FolderSizeCalculator calculator(useParallelProcessing, maxThreads);
    
    if (!timeOnly) {
        std::cout << "Calculating folder sizes for: " << rootPath << std::endl;
        if (useParallelProcessing) {
            std::cout << "Using parallel processing with " 
                      << (maxThreads > 0 ? std::to_string(maxThreads) : "auto-detected") 
                      << " threads" << std::endl;
        } else {
            std::cout << "Using sequential processing" << std::endl;
        }
    }
    
    auto result = calculator.calculateFolderSizes(rootPath);
    
    if (!result.rootNode) {
        std::cerr << "Failed to process directory." << std::endl;
        return 1;
    }
    
    if (timeOnly) {
        // Display only the time taken
        std::cout << result.elapsedTimeMs << std::endl;
    } else {
        // Display full results
        std::cout << "\nResults:\n";
        printTree(result.rootNode);
        
        std::cout << "\nTotal size: " << formatSize(result.rootNode->size) << std::endl;
        std::cout << "Time taken: " << std::fixed << std::setprecision(2) 
                  << result.elapsedTimeMs << " ms" << std::endl;
    }
    
    return 0;
} 