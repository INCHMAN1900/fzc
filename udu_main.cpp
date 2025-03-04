#include "udu_lib.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <cstring>
#include <locale>
#include <codecvt>

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
    std::cout << "Usage: udu [options] <path>\n"
              << "Options:\n"
              << "  -t, --time-only    Display only the time taken for calculation\n"
              << "  -s, --sequential   Use sequential processing (disable parallel processing)\n"
              << "  -j, --threads N    Specify maximum number of threads to use (default: auto)\n"
              << "  -h, --help         Display this help message\n\n"
              << "The path can be either a directory or a single file.\n";
}

void printNode(const std::shared_ptr<FileNode>& node, int level = 0, bool timeOnly = false) {
    if (timeOnly) return;
    
    std::string indent(level * 2, ' ');
    std::cout << indent << node->path << " (" << formatSize(node->size) << ")\n";
    
    for (const auto& child : node->children) {
        printNode(child, level + 1, timeOnly);
    }
}

int main(int argc, char* argv[]) {
    // Set up locale for UTF-8 output
    std::ios_base::sync_with_stdio(false);
    try {
        std::locale::global(std::locale("en_US.UTF-8"));
        std::cout.imbue(std::locale());
        std::cerr.imbue(std::locale());
    } catch (const std::exception& e) {
        // Continue even if locale setting fails
        std::cerr << "Warning: Could not set locale: " << e.what() << std::endl;
    }
    
    if (argc < 2) {
        printUsage();
        return 1;
    }
    
    // Parse command line arguments
    std::string directoryPath;
    bool useParallelProcessing = true;
    int maxThreads = 0;
    bool timeOnly = false;
    
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
                std::cerr << "Error: Multiple paths specified\n";
                printUsage();
                return 1;
            }
            directoryPath = arg;
        }
    }
    
    if (directoryPath.empty()) {
        std::cerr << "Error: No path specified\n";
        printUsage();
        return 1;
    }
    
    try {
        // Create calculator with specified settings
        UduCalculator calculator(useParallelProcessing, maxThreads);
        
        // Calculate sizes
        auto result = calculator.calculateFolderSizes(directoryPath);
        
        if (!result.rootNode) {
            std::cerr << "Error: Failed to process path: " << directoryPath << "\n";
            return 1;
        }
        
        // Print results
        if (!timeOnly) {
            std::cout << "\nResults for: " << directoryPath << "\n\n";
            if (result.rootNode->isDirectory) {
                printNode(result.rootNode, 0, timeOnly);
            } else {
                std::cout << result.rootNode->path << " (" << formatSize(result.rootNode->size) << ")\n";
            }
            std::cout << "\nTotal size: " << formatSize(result.rootNode->size) << "\n";
        }
        
        std::cout << "Time taken: " << result.elapsedTimeMs << " ms\n";
        
        return 0;
    } catch (const std::runtime_error& e) {
        // Handle specific runtime errors (like path too long)
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        // Handle other exceptions
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        // Handle unknown exceptions
        std::cerr << "Error: An unknown error occurred\n";
        return 1;
    }
} 