/**
 * @file cache_utils.cpp
 * @brief CPU Cache Detection and Optimization Implementation
 */

#include "cache_utils.hpp"

#include <fstream>
#include <sstream>
#include <dirent.h>
#include <sched.h>
#include <unistd.h>
#include <cstring>

namespace SIM {

// Default cache sizes if detection fails
constexpr size_t DEFAULT_L1_SIZE = 32 * 1024;      // 32 KB
constexpr size_t DEFAULT_L2_SIZE = 256 * 1024;     // 256 KB
constexpr size_t DEFAULT_L3_SIZE = 8 * 1024 * 1024; // 8 MB

CacheInfo CacheUtils::detectCacheInfo() {
    CacheInfo info;
    info.l1d_size = DEFAULT_L1_SIZE;
    info.l1i_size = DEFAULT_L1_SIZE;
    info.l2_size = DEFAULT_L2_SIZE;
    info.l3_size = DEFAULT_L3_SIZE;
    info.line_size = CACHE_LINE_SIZE;
    info.num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    
    // Try to read from sysfs
    const std::string cache_base = "/sys/devices/system/cpu/cpu0/cache/";
    
    // Scan cache index directories
    DIR* dir = opendir(cache_base.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "index", 5) == 0) {
                std::string index_path = cache_base + entry->d_name + "/";
                
                // Read cache type
                std::ifstream type_file(index_path + "type");
                std::string type;
                if (type_file >> type) {
                    // Read cache level
                    std::ifstream level_file(index_path + "level");
                    int level = 0;
                    level_file >> level;
                    
                    // Read cache size
                    size_t size = readCacheSize(index_path + "size");
                    
                    // Read line size
                    std::ifstream line_file(index_path + "coherency_line_size");
                    size_t line = 0;
                    if (line_file >> line && line > 0) {
                        info.line_size = line;
                    }
                    
                    // Assign based on level and type
                    if (level == 1 && type == "Data") {
                        info.l1d_size = size;
                    } else if (level == 1 && type == "Instruction") {
                        info.l1i_size = size;
                    } else if (level == 2) {
                        info.l2_size = size;
                    } else if (level == 3) {
                        info.l3_size = size;
                    }
                }
            }
        }
        closedir(dir);
    }
    
    return info;
}

size_t CacheUtils::readCacheSize(const std::string& path) {
    std::ifstream file(path);
    std::string str;
    if (file >> str) {
        return parseSize(str);
    }
    return 0;
}

size_t CacheUtils::parseSize(const std::string& str) {
    size_t value = 0;
    size_t multiplier = 1;
    
    std::stringstream ss(str);
    ss >> value;
    
    // Check for suffix
    char suffix = 0;
    if (ss >> suffix) {
        switch (suffix) {
            case 'K': case 'k': multiplier = 1024; break;
            case 'M': case 'm': multiplier = 1024 * 1024; break;
            case 'G': case 'g': multiplier = 1024 * 1024 * 1024; break;
        }
    }
    
    return value * multiplier;
}

HugePagesInfo CacheUtils::detectHugePages() {
    HugePagesInfo info;
    info.available = false;
    info.usable = false;
    info.total = 0;
    info.free = 0;
    info.page_size = HUGE_PAGE_SIZE;
    
    // Read from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    
    while (std::getline(meminfo, line)) {
        if (line.find("HugePages_Total:") == 0) {
            std::sscanf(line.c_str(), "HugePages_Total: %zu", &info.total);
        } else if (line.find("HugePages_Free:") == 0) {
            std::sscanf(line.c_str(), "HugePages_Free: %zu", &info.free);
        } else if (line.find("Hugepagesize:") == 0) {
            size_t size_kb = 0;
            std::sscanf(line.c_str(), "Hugepagesize: %zu kB", &size_kb);
            info.page_size = size_kb * 1024;
        }
    }
    
    info.available = (info.total > 0);
    info.usable = (info.free > 0);
    
    return info;
}

NUMAInfo CacheUtils::detectNUMA() {
    NUMAInfo info;
    info.available = false;
    info.num_nodes = 1;
    info.current_node = 0;
    
    // Check for NUMA nodes
    DIR* dir = opendir("/sys/devices/system/node/");
    if (dir) {
        struct dirent* entry;
        int node_count = 0;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "node", 4) == 0) {
                ++node_count;
            }
        }
        closedir(dir);
        
        if (node_count > 1) {
            info.available = true;
            info.num_nodes = node_count;
        }
    }
    
    // Try to get current node (requires libnuma, fallback to 0)
    // For portability, we don't require libnuma
    info.current_node = 0;
    
    return info;
}

bool CacheUtils::fitsInL3(size_t size) {
    auto cache = detectCacheInfo();
    return size <= cache.l3_size / 2;  // Leave room for other data
}

bool CacheUtils::shouldUseHugePages(size_t size) {
    // Huge pages beneficial for allocations > 1MB
    if (size < 1024 * 1024) {
        return false;
    }
    
    auto hp = detectHugePages();
    if (!hp.usable) {
        return false;
    }
    
    // Check if we have enough free huge pages
    size_t pages_needed = (size + hp.page_size - 1) / hp.page_size;
    return pages_needed <= hp.free;
}

size_t CacheUtils::alignToCacheLine(size_t size) {
    return ((size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;
}

size_t CacheUtils::alignToHugePage(size_t size) {
    return ((size + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE) * HUGE_PAGE_SIZE;
}

void CacheUtils::prefetchRange(const void* addr, size_t size) {
    const char* ptr = static_cast<const char*>(addr);
    for (size_t i = 0; i < size; i += CACHE_LINE_SIZE) {
        __builtin_prefetch(ptr + i, 0, 1);
    }
}

bool CacheUtils::setCpuAffinity(int cpu_id) {
    if (cpu_id < 0) {
        return true;  // No change requested
    }
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
}

int CacheUtils::getCurrentCpu() {
    return sched_getcpu();
}

} // namespace SIM
