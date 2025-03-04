# FZC (Fast Size Calculator)

A high-performance C++ library for calculating folder sizes with parallel processing and memory mapping optimizations.

## Features

- Fast parallel directory traversal
- Memory mapping for large files
- Thread pool for efficient parallel processing
- Cycle detection to handle symbolic links
- Batch processing for better performance
- C interface for easy integration with other languages
- Swift integration example included

## Building

```bash
mkdir build
cd build
cmake ..
make
```

## Installation

```bash
sudo make install
```

## Usage

### C++ Example

```cpp
#include <fzc.hpp>
#include <iostream>

int main() {
    // Create calculator with default settings (parallel processing enabled)
    FZC calculator;
    
    // Calculate sizes
    auto result = calculator.calculateFolderSizes("/path/to/folder");
    
    // Print results
    std::cout << "Total size: " << result.rootNode->size << " bytes\n";
    std::cout << "Time taken: " << result.elapsedTimeMs << " ms\n";
    
    return 0;
}
```

### Swift Example

```swift
import fzc

let calculator = FZC()
if let result = calculator.calculateSizes(path: "/path/to/folder") {
    print("Total size: \(result.rootNode.size) bytes")
    print("Time taken: \(result.elapsedTimeMs) ms")
}
```

## Performance Optimizations

1. **Parallel Processing**: Uses a thread pool for efficient parallel directory traversal
2. **Memory Mapping**: Large files are memory mapped for faster size calculation
3. **Batch Processing**: Directory entries are processed in batches to reduce overhead
4. **Early Path Filtering**: Detects and prevents cycles in directory traversal
5. **Efficient Memory Management**: Uses smart pointers and move semantics

## Requirements

- C++17 or later
- CMake 3.15 or later
- macOS 11.0 or later (for Intel and ARM64 architectures)

## License

MIT License 