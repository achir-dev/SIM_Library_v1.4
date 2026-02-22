/**
 * @file cache_utils.hpp
 * @brief CPU Cache Detection and Optimization Utilities
 * 
 * Provides automatic detection of:
 * - Cache sizes (L1, L2, L3)
 * - Cache line size
 * - Huge pages availability
 * - NUMA topology
 * 
 * All detection is portable and gracefully handles missing features.
 */

#ifndef CACHE_UTILS_HPP
#define CACHE_UTILS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace SIM {

// Cache line size (64 bytes on modern x86_64)
constexpr size_t CACHE_LINE_SIZE = 64;

// Huge page size (2MB on x86_64)
constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;

/**
 * @struct CacheInfo
 * @brief Information about CPU cache hierarchy
 */
struct CacheInfo {
    size_t l1d_size;        // L1 data cache size (bytes)
    size_t l1i_size;        // L1 instruction cache size (bytes)
    size_t l2_size;         // L2 cache size (bytes)
    size_t l3_size;         // L3 cache size (bytes)
    size_t line_size;       // Cache line size (bytes)
    int num_cores;          // Number of CPU cores
    
    // Derived values
    size_t optimal_prefetch_distance() const {
        return l2_size > 0 ? l2_size / 4 : 64 * 1024;
    }
    
    size_t optimal_chunk_size() const {
        return l3_size > 0 ? l3_size / 2 : 1024 * 1024;
    }
};

/**
 * @struct HugePagesInfo
 * @brief Information about huge pages availability
 */
struct HugePagesInfo {
    bool available;         // Huge pages supported
    bool usable;            // Enough free huge pages
    size_t total;           // Total huge pages
    size_t free;            // Free huge pages
    size_t page_size;       // Huge page size (usually 2MB)
};

/**
 * @struct NUMAInfo
 * @brief NUMA topology information
 */
struct NUMAInfo {
    bool available;         // NUMA system detected
    int num_nodes;          // Number of NUMA nodes
    int current_node;       // Current CPU's NUMA node
};

/**
 * @class CacheUtils
 * @brief Static utilities for cache detection and optimization
 */
class CacheUtils {
public:
    /**
     * @brief Detect CPU cache information
     * @return CacheInfo with detected or default values
     */
    static CacheInfo detectCacheInfo();
    
    /**
     * @brief Detect huge pages availability
     * @return HugePagesInfo with current status
     */
    static HugePagesInfo detectHugePages();
    
    /**
     * @brief Detect NUMA topology
     * @return NUMAInfo with current topology
     */
    static NUMAInfo detectNUMA();
    
    /**
     * @brief Check if payload fits in L3 cache
     */
    static bool fitsInL3(size_t size);
    
    /**
     * @brief Check if huge pages are beneficial for size
     * Huge pages are beneficial for allocations > 2MB
     */
    static bool shouldUseHugePages(size_t size);
    
    /**
     * @brief Align size up to cache line boundary
     */
    static size_t alignToCacheLine(size_t size);
    
    /**
     * @brief Align size up to huge page boundary
     */
    static size_t alignToHugePage(size_t size);
    
    /**
     * @brief Prefetch memory for reading
     */
    static inline void prefetchRead(const void* addr) {
        __builtin_prefetch(addr, 0, 3);  // Read, high locality
    }
    
    /**
     * @brief Prefetch memory for writing
     */
    static inline void prefetchWrite(void* addr) {
        __builtin_prefetch(addr, 1, 3);  // Write, high locality
    }
    
    /**
     * @brief Prefetch range of memory
     */
    static void prefetchRange(const void* addr, size_t size);
    
    /**
     * @brief Set CPU affinity for current thread
     * @param cpu_id CPU core to pin to (-1 for no change)
     * @return true if successful
     */
    static bool setCpuAffinity(int cpu_id);
    
    /**
     * @brief Get current CPU core
     */
    static int getCurrentCpu();

private:
    static size_t readCacheSize(const std::string& path);
    static size_t parseSize(const std::string& str);
};

/**
 * @struct SiCConfig
 * @brief Configuration for SIM Turbo
 */
struct SiCConfig {
    bool use_huge_pages;        // Try to use huge pages
    bool enable_prefetch;       // Enable software prefetching
    bool numa_aware;            // NUMA-aware allocation
    int cpu_affinity;           // CPU to pin to (-1 = none)
    size_t prefetch_distance;   // Prefetch distance (0 = auto)
    
    /**
     * @brief Create config with auto-detected optimal settings
     */
    static SiCConfig autoDetect() {
        SiCConfig cfg;
        auto cache = CacheUtils::detectCacheInfo();
        auto hp = CacheUtils::detectHugePages();
        
        cfg.use_huge_pages = hp.usable;
        cfg.enable_prefetch = true;
        cfg.numa_aware = true;
        cfg.cpu_affinity = -1;
        cfg.prefetch_distance = cache.optimal_prefetch_distance();
        
        return cfg;
    }
    
    /**
     * @brief Create config for maximum portability (no special features)
     */
    static SiCConfig portable() {
        SiCConfig cfg;
        cfg.use_huge_pages = false;
        cfg.enable_prefetch = true;
        cfg.numa_aware = false;
        cfg.cpu_affinity = -1;
        cfg.prefetch_distance = 64 * 1024;
        return cfg;
    }
    
    /**
     * @brief Create config for maximum performance
     */
    static SiCConfig maxPerformance() {
        SiCConfig cfg;
        cfg.use_huge_pages = true;
        cfg.enable_prefetch = true;
        cfg.numa_aware = true;
        cfg.cpu_affinity = 0;  // Pin to core 0
        cfg.prefetch_distance = 0;  // Auto
        return cfg;
    }
};

/**
 * @struct SiCStats
 * @brief Runtime statistics for SIM Turbo
 */
struct SiCStats {
    // Configuration used
    bool huge_pages_active;
    bool prefetch_active;
    int numa_node;
    int pinned_cpu;
    
    // Detected cache info
    CacheInfo cache_info;
    
    // Performance counters
    uint64_t total_writes;
    uint64_t total_reads;
    uint64_t bytes_transferred;
};

} // namespace SIM

#endif // CACHE_UTILS_HPP
