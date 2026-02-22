/**
 * @file casir.hpp
 * @brief CASIR (Cache Access Streaming Into Reader) - Cache-Optimized Ultra-Low Latency Transport
 * 
 * Enhanced version of SIM with:
 * - Huge pages support (2MB pages, auto-fallback)
 * - Cache line aligned structures
 * - Software prefetching
 * - NUMA awareness
 * - CPU affinity support
 * 
 * All features auto-detect and fallback gracefully.
 */

#ifndef CASIR_HPP
#define CASIR_HPP

#include "cache_utils.hpp"
#include <string>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>

namespace CASIR {

// Import types from SIM namespace
using Config = SIM::SiCConfig;
using Stats = SIM::SiCStats;
using CacheInfo = SIM::CacheInfo;
using CacheUtils = SIM::CacheUtils;
constexpr size_t CACHE_LINE_SIZE = SIM::CACHE_LINE_SIZE;
constexpr size_t HUGE_PAGE_SIZE = SIM::HUGE_PAGE_SIZE;

// Magic number for header validation
constexpr uint32_t CASIR_MAGIC = 0x43415352;  // "CASR"

// Version
constexpr uint32_t CASIR_VERSION = 0x00010000;

/**
 * @struct Header
 * @brief Cache-line aligned header for CASIR (Cache Access Streaming Into Reader)
 * 
 * Each atomic field has its own cache line to prevent false sharing.
 */
struct alignas(CACHE_LINE_SIZE) Header {
    // === Cache Line 0: Static metadata ===
    uint32_t magic;
    uint32_t version;
    size_t capacity;
    size_t huge_page_size;
    uint32_t flags;
    char padding0[CACHE_LINE_SIZE - sizeof(uint32_t)*3 - sizeof(size_t)*2];
    
    // === Cache Line 1: Front index (hot, written by writer) ===
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> front_idx;
    char padding1[CACHE_LINE_SIZE - sizeof(std::atomic<uint32_t>)];
    
    // === Cache Line 2: Buffer 0 metadata ===
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> frame0;
    std::atomic<int64_t> timestamp0_ns;
    std::atomic<uint32_t> checksum0;
    char padding2[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>) - 
                  sizeof(std::atomic<int64_t>) - sizeof(std::atomic<uint32_t>)];
    
    // === Cache Line 3: Buffer 1 metadata ===
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> frame1;
    std::atomic<int64_t> timestamp1_ns;
    std::atomic<uint32_t> checksum1;
    char padding3[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>) - 
                  sizeof(std::atomic<int64_t>) - sizeof(std::atomic<uint32_t>)];
    
    // === Cache Line 4: Writer state ===
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> published_length;
    std::atomic<int64_t> writer_heartbeat_ns;
    std::atomic<bool> checksum_enabled;
    char padding4[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - 
                  sizeof(std::atomic<int64_t>) - sizeof(std::atomic<bool>)];
    
    // === Cache Line 5: Stats ===
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> total_writes;
    std::atomic<uint64_t> total_bytes;
    char padding5[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)*2];
};

// Verify cache line alignment
static_assert(sizeof(Header) % CACHE_LINE_SIZE == 0, 
              "Header must be cache-line aligned");

/**
 * @class Writer
 * @brief Cache-optimized writer for ultra-low latency
 */
class Writer {
public:
    /**
     * @brief Constructor with auto-detected configuration
     */
    Writer(const std::string& shm_name, size_t max_size,
                const Config& config = Config::autoDetect());
    
    ~Writer();
    
    // Non-copyable
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    
    // Movable
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;
    
    /**
     * @brief Initialize shared memory with optimal settings
     */
    bool init();
    
    /**
     * @brief Write data with prefetching optimization
     */
    bool write(const void* data, size_t size);
    
    /**
     * @brief Zero-copy write - caller fills buffer directly
     * @param fill_func Function that fills the buffer
     * @param size Size of data to write
     */
    bool writeZeroCopy(std::function<void(void*)> fill_func, size_t size);
    
    /**
     * @brief Get direct pointer to back buffer for zero-copy
     * Call commitWrite() after filling
     */
    void* getWriteBuffer();
    
    /**
     * @brief Commit after filling buffer from getWriteBuffer()
     */
    bool commitWrite(size_t size);
    
    bool isReady() const { return is_initialized_; }
    const std::string& getName() const { return shm_name_; }
    uint64_t getFrameCount() const { return frame_count_; }
    Stats getStats() const;
    
    void destroy();

private:
    std::string shm_name_;
    size_t max_size_;
    Config config_;
    bool is_initialized_;
    
    int shm_fd_;
    void* shm_ptr_;
    size_t shm_size_;
    bool using_huge_pages_;
    
    Header* header_;
    uint8_t* buffer_[2];
    uint64_t frame_count_;
    
    CacheInfo cache_info_;
    
    bool allocateMemory();
    void prefetchBuffer(int idx);
    uint32_t calculateChecksum(const void* data, size_t size) const;
    int64_t getCurrentTimestampNs() const;
};

/**
 * @class Reader
 * @brief Cache-optimized reader for ultra-low latency
 */
class Reader {
public:
    /**
     * @brief Constructor with auto-detected configuration
     */
    Reader(const std::string& shm_name, size_t max_size,
                const Config& config = Config::autoDetect());
    
    ~Reader();
    
    // Non-copyable
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    
    // Movable
    Reader(Reader&& other) noexcept;
    Reader& operator=(Reader&& other) noexcept;
    
    /**
     * @brief Initialize connection to shared memory
     */
    bool init();
    
    /**
     * @brief Read with prefetching optimization
     */
    bool read(void* data, size_t& size);
    
    /**
     * @brief Zero-copy read - returns pointer to buffer
     * MUST call releaseZeroCopy() after processing
     */
    const void* readZeroCopy(size_t& size);
    
    /**
     * @brief Release zero-copy read lock
     */
    void releaseZeroCopy();
    
    /**
     * @brief Read with timeout
     */
    bool readWithTimeout(void* data, size_t& size, uint32_t timeout_ms);
    
    bool isReady() const { return is_initialized_; }
    bool isWriterAlive(uint32_t timeout_ms = 1000) const;
    
    int64_t getLastTimestampNs() const { return last_timestamp_ns_; }
    uint64_t getLastFrameNumber() const { return last_frame_; }
    uint64_t getDroppedFrames() const { return dropped_frames_; }
    bool verifyLastChecksum() const { return last_checksum_valid_; }
    
    Stats getStats() const;

private:
    std::string shm_name_;
    size_t max_size_;
    Config config_;
    bool is_initialized_;
    
    int shm_fd_;
    void* shm_ptr_;
    size_t shm_size_;
    bool using_huge_pages_;
    
    Header* header_;
    const uint8_t* buffer_[2];
    
    uint64_t last_frame_;
    int64_t last_timestamp_ns_;
    uint64_t dropped_frames_;
    bool last_checksum_valid_;
    bool zero_copy_active_;
    
    CacheInfo cache_info_;
    
    void prefetchBuffer(int idx);
    uint32_t calculateChecksum(const void* data, size_t size) const;
    int64_t getCurrentTimestampNs() const;
};

} // namespace CASIR

#endif // CASIR_HPP
