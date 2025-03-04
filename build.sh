#!/bin/bash

# Exit on error
set -e

# Print commands before executing
set -x

# Clean up existing build directory
rm -rf build

# Create build directory
mkdir build

# Change to build directory
cd build

# Configure with CMake
cmake ..

# Build
cmake --build . -j$(sysctl -n hw.ncpu)

echo "Build completed successfully!"
echo "The library 'libfzc.dylib' and executable 'fzc_cli' are in the build directory." 