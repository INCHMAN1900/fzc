#include "fzc.hpp"
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
void printUsage() {
    std::cout << "Usage: fzc_cli [options] <directory_path>\n"
              << "Options:\n"
              << "  -t, --time-only    Display only the time taken for calculation\n"
              << "  -s, --sequential   Use sequential processing (disable parallel processing)\n"
              << "  -j, --threads N    Specify maximum number of threads to use (default: auto)\n"
              << "  -r, --root-only    Only calculate the size of the root directory\n"
              << "  -h, --help         Display this help message\n";
}

void printNode(const std::shared_ptr<FileNode>& node, int level = 0, bool timeOnly = false) {
    if (timeOnly) return;
    
    std::string indent(level * 2, ' ');
    std::cout << indent << node->path << " (" << node->size << " bytes)\n";
    
    for (const auto& child : node->children) {
        printNode(child, level + 1, timeOnly);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    
    // Parse command line arguments
    std::string directoryPath;
    bool useParallelProcessing = true;
    int maxThreads = 0;
    bool timeOnly = false;
    bool rootOnly = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        else if (arg == "-t" || arg == "--time-only") {
            timeOnly = true;
        }
        else if (arg == "-s" || arg == "--sequential") {
            useParallelProcessing = false;
        }
        else if (arg == "-r" || arg == "--root-only") {
            rootOnly = true;
        }
        else if (arg == "-j" || arg == "--threads") {
            if (i + 1 < argc) {
                try {
                    maxThreads = std::stoi(argv[++i]);
                    if (maxThreads < 0) {
                        std::cerr << "Error: Thread count must be non-negative\n";
                        return 1;
                    }
                } catch (const std::exception&) {
                    std::cerr << "Error: Invalid thread count\n";
                    return 1;
                }
            } else {
                std::cerr << "Error: -j/--threads requires a number\n";
                return 1;
            }
        }
        else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage();
            return 1;
        }
        else {
            if (!directoryPath.empty()) {
                std::cerr << "Error: Multiple directory paths specified\n";
                printUsage();
                return 1;
            }
            directoryPath = arg;
        }
    }
    
    if (directoryPath.empty()) {
        std::cerr << "Error: No directory path specified\n";
        printUsage();
        return 1;
    }
    
    // Create calculator with specified settings
    FZC calculator(useParallelProcessing, maxThreads, false);
    
    // Calculate sizes
    auto result = calculator.calculateFolderSizes(directoryPath, rootOnly);
    
    // Print results
    if (!timeOnly) {
        std::cout << "\nResults for: " << directoryPath << "\n\n";
        printNode(result.rootNode, 0, timeOnly);
        std::cout << "\nTotal size: " << result.rootNode->size << " bytes\n";
    }
    
    std::cout << "Time taken: " << result.elapsedTimeMs << " ms\n";
    
    return 0;
} 