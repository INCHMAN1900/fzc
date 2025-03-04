# UDU - Universal Disk Usage

A C++ library and command-line tool for efficiently calculating folder sizes that can be integrated into macOS applications.

## Features

- Fast directory traversal and size calculation using parallel processing
- Automatic thread management for optimal performance
- Tree structure representation of files and folders
- Performance timing information
- C-style interface for easy integration with Swift
- Error handling for inaccessible files and directories
- Universal binary supporting both Intel and ARM64 Macs

## Building the Library

### Prerequisites

- CMake 3.12 or higher
- C++17 compatible compiler (Clang on macOS)
- macOS 11.0 or higher

### Build Steps

1. Clone or download this repository
2. Open Terminal and navigate to the project directory
3. Run the following commands:

```bash
mkdir build
cd build
cmake ..
make
```

This will generate:
- `folder_size_calculator.dylib` - The shared library
- `udu` - The command-line executable

### Using the Command-Line Tool

You can use the `udu` command-line tool to analyze directory sizes:

```bash
./udu /path/to/directory
```

The output will show:
- A tree structure of files and directories with their sizes
- The total size of the specified directory
- The time taken to perform the calculation

#### Command-Line Options

The `udu` tool supports the following options:

```bash
./udu [options] /path/to/directory
```

Options:
- `-t, --time-only`: Display only the time taken for calculation, omitting the directory structure and sizes
- `-s, --sequential`: Use sequential processing (disable parallel processing)
- `-j, --threads <num>`: Specify maximum number of threads to use (default: auto-detect)
- `-h, --help`: Display help information

Examples:

```bash
# Display full results using parallel processing (default)
./udu /path/to/directory

# Display only the time taken for calculation
./udu -t /path/to/directory

# Use sequential processing (no parallelism)
./udu -s /path/to/directory

# Specify the number of threads to use
./udu -j 4 /path/to/directory
```

## Performance Optimization

The tool uses parallel processing by default to maximize performance:

- Automatically detects the number of available CPU cores
- Creates a thread pool for parallel directory traversal
- Limits parallelism to the top levels of the directory tree for efficiency
- Sorts results by size for better visualization

For the best performance:
- Let the tool auto-detect the number of threads (default behavior)
- For very large directory structures, you may want to experiment with different thread counts

## Integration with macOS App

### Option 1: Using the Swift Wrapper

1. Copy `folder_size_calculator.dylib` to your macOS app's bundle resources
2. Add `FolderSizeCalculator.swift` to your Xcode project
3. Use the `FolderSizeCalculator` class as shown in the example at the bottom of the Swift file

### Option 2: Using Xcode's Build System

1. Add the C++ source files to your Xcode project
2. Create a bridging header to expose the C functions to Swift
3. Configure your project to build the C++ files with C++17 support

### Example Integration Steps in Xcode

1. Create a new macOS app project in Xcode
2. Add the C++ files (`folder_size_calculator.cpp` and `folder_size_calculator.hpp`) to your project
3. Create a bridging header file (e.g., `BridgingHeader.h`) with the following content:

```c
#ifndef BridgingHeader_h
#define BridgingHeader_h

#include "folder_size_calculator.hpp"

#endif /* BridgingHeader_h */
```

4. In your project settings, under "Build Settings":
   - Set "C++ Language Dialect" to "C++17 [-std=c++17]"
   - Set "C++ Standard Library" to "libc++ (LLVM C++ standard library with C++11 support)"
   - Set "Objective-C Bridging Header" to the path of your bridging header

5. Use the Swift wrapper class or directly call the C functions from your Swift code

### Configuring Parallel Processing in Swift

The Swift wrapper allows you to configure parallel processing:

```swift
// Create calculator with parallel processing (using all available cores)
let calculator = FolderSizeCalculator(useParallelProcessing: true, maxThreads: 0)

// Create calculator with parallel processing (using specific number of threads)
let calculator = FolderSizeCalculator(useParallelProcessing: true, maxThreads: 4)

// Create calculator with sequential processing (no parallelism)
let calculator = FolderSizeCalculator(useParallelProcessing: false)

// Calculate folder sizes
if let result = calculator.calculateFolderSizes(at: path) {
    // Access the root node
    let rootNode = result.rootNode
    
    // Access timing information
    let timeMs = result.elapsedTimeMs
    print("Calculation took \(timeMs) ms")
}
```

## Memory Management

- The C++ library uses smart pointers internally
- The C interface handles memory management through explicit release functions
- The Swift wrapper properly releases C++ resources in its deinit method

## Compatibility

- Supports macOS 11.0 and later
- Universal binary for both Intel (x86_64) and Apple Silicon (arm64) Macs

## License

This project is available under the MIT License. 