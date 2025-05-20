#include <iostream>
#include <filesystem>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <sys/attr.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace fs = std::filesystem;

struct FileEntry {
    fs::path path;
    uint64_t size = 0;
    std::vector<FileEntry> children;
};

uint64_t getAllocatedSize(const std::string& path) {
    // 使用原始字节数组避免结构体填充问题
    char buf[sizeof(uint32_t) + sizeof(uint64_t)] = {0};

    struct attrlist attrList = {};
    attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrList.fileattr = ATTR_FILE_ALLOCSIZE;

    // 设置 length 字段
    *reinterpret_cast<uint32_t*>(buf) = sizeof(buf);

    if (getattrlist(path.c_str(), &attrList, buf, sizeof(buf), 0) != 0) {
        std::cerr << "getattrlist failed on " << path << ": " << strerror(errno) << "\n";
        return 0;
    }

    // 读取 allocsize 字段（紧跟 length 之后）
    uint64_t allocsize = *reinterpret_cast<uint64_t*>(buf + sizeof(uint32_t));
    // std::cout << "DEBUG: " << path << " allocsize=" << allocsize << std::endl;
    return allocsize;
}

// 过滤：是否为挂载点
bool isMountPoint(const fs::path& path) {
    struct stat parentStat, selfStat;
    auto parent = path.parent_path();
    if (stat(parent.c_str(), &parentStat) != 0) return false;
    if (stat(path.c_str(), &selfStat) != 0) return false;
    return parentStat.st_dev != selfStat.st_dev;
}

// 记录已访问过的 inode（防止硬链接重复计数）
std::unordered_set<uint64_t> visitedInodes;

FileEntry scanDirectory(const fs::path& path) {
    FileEntry entry;
    entry.path = path;

    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return entry;

    // 跳过符号链接
    if (S_ISLNK(st.st_mode)) return entry;

    // 跳过挂载点
    if (fs::is_directory(path) && isMountPoint(path)) return entry;

    uint64_t inode = st.st_ino;
    if (!S_ISDIR(st.st_mode)) {
        // 非目录：检测 inode 去重
        if (visitedInodes.find(inode) != visitedInodes.end()) return entry;
        visitedInodes.insert(inode);
        entry.size = getAllocatedSize(path); // 只对普通文件调用
        return entry;
    }

    // 目录：递归遍历子项，不调用 getAllocatedSize
    uint64_t totalSize = 0;
    for (const auto& p : fs::directory_iterator(path)) {
        auto child = scanDirectory(p.path());
        totalSize += child.size;
        entry.children.push_back(std::move(child));
    }

    entry.size = totalSize;
    return entry;
}

void printTree(const FileEntry& entry, int indent = 0) {
    std::cout << std::string(indent, ' ')
              << entry.path.filename().string()
              << ": " << entry.size << " bytes"
              << " (" << entry.size / 1024.0 << " KB"
              << ", " << entry.size / 1000.0 << " KB[1000])"
              << std::endl;
    for (const auto& child : entry.children) {
        printTree(child, indent + 2);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: scanner /path/to/dir\n";
        return 1;
    }
    fs::path root = argv[1];
    if (!fs::exists(root)) {
        std::cerr << "Path not found.\n";
        return 1;
    }

    FileEntry rootEntry = scanDirectory(root);
    printTree(rootEntry);
    return 0;
}
