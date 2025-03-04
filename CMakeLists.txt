cmake_minimum_required(VERSION 3.15)
project(fzc VERSION 1.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Set macOS deployment target and architecture support
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")

# Add library target
add_library(fzc SHARED
    fzc.cpp
)

# Set properties for the library
set_target_properties(fzc PROPERTIES
    OUTPUT_NAME "fzc"
    PREFIX ""
    SUFFIX ".dylib"
    PUBLIC_HEADER fzc.hpp
    MACOSX_RPATH ON
)

# Add executable target
add_executable(fzc_cli main.cpp)
target_link_libraries(fzc_cli PRIVATE fzc)

# Installation rules
include(GNUInstallDirs)
install(TARGETS fzc fzc_cli
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Code signing for macOS
if(APPLE)
    add_custom_command(TARGET fzc POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:fzc>
        COMMENT "Signing the library"
    )
    add_custom_command(TARGET fzc_cli POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:fzc_cli>
        COMMENT "Signing the executable"
    )
endif() 