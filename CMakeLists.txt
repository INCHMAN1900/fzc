cmake_minimum_required(VERSION 3.12)
project(FolderSizeCalculator)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set macOS deployment target and architecture support
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")

# Add library target
add_library(folder_size_calculator SHARED
    folder_size_calculator.cpp
    folder_size_calculator.hpp
)

# Add executable (renamed to udu)
add_executable(udu test_calculator.cpp)
target_link_libraries(udu folder_size_calculator)

# Set output properties for macOS
set_target_properties(folder_size_calculator PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
    PUBLIC_HEADER "folder_size_calculator.hpp"
    MACOSX_RPATH ON
)

# Installation rules
install(TARGETS folder_size_calculator udu
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
    RUNTIME DESTINATION bin
) 