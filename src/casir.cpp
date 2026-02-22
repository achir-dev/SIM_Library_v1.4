/**
 * @file sic.cpp
 * @brief CASIR (Cache Access Streaming Into Reader) Implementation - Cache-Optimized Ultra-Low Latency
 */

#include "casir.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <chrono>
#include <thread>

namespace CASIR {

// ============================================================================
// Writer Implementation
// ============================================================================

Writer::Writer(const std::string& shm_name, size_t max_size,
                         const Config& config)
    : shm_name_(shm_name)
    , max_size_(max_size)
    , config_(config)
    , is_initialized_(false)
    , shm_fd_(-1)
    , shm_ptr_(nullptr)
    , shm_size_(0)
    , using_huge_pages_(false)
    , header_(nullptr)
    , frame_count_(0)
{
    buffer_[0] = nullptr;
    buffer_[1] = nullptr;
    cache_info_ = CacheUtils::detectCacheInfo();
    
    // Auto-configure prefetch distance if not set
    if (config_.prefetch_distance == 0) {
        config_.prefetch_distance = cache_info_.optimal_prefetch_distance();
    }
}

Writer::~Writer() {
    destroy();
}

Writer::Writer(Writer&& other) noexcept
    : shm_name_(std::move(other.shm_name_))
    , max_size_(other.max_size_)
    , config_(other.config_)
    , is_initialized_(other.is_initialized_)
    , shm_fd_(other.shm_fd_)
    , shm_ptr_(other.shm_ptr_)
    , shm_size_(other.shm_size_)
    , using_huge_pages_(other.using_huge_pages_)
    , header_(other.header_)
    , frame_count_(other.frame_count_)
    , cache_info_(other.cache_info_)
{
    buffer_[0] = other.buffer_[0];
    buffer_[1] = other.buffer_[1];
    
    other.is_initialized_ = false;
    other.shm_fd_ = -1;
    other.shm_ptr_ = nullptr;
    other.header_ = nullptr;
}

Writer& Writer::operator=(Writer&& other) noexcept {
    if (this != &other) {
        destroy();
        
        shm_name_ = std::move(other.shm_name_);
        max_size_ = other.max_size_;
        config_ = other.config_;
        is_initialized_ = other.is_initialized_;
        shm_fd_ = other.shm_fd_;
        shm_ptr_ = other.shm_ptr_;
        shm_size_ = other.shm_size_;
        using_huge_pages_ = other.using_huge_pages_;
        header_ = other.header_;
        buffer_[0] = other.buffer_[0];
        buffer_[1] = other.buffer_[1];
        frame_count_ = other.frame_count_;
        cache_info_ = other.cache_info_;
        
        other.is_initialized_ = false;
        other.shm_fd_ = -1;
        other.shm_ptr_ = nullptr;
        other.header_ = nullptr;
    }
    return *this;
}

bool Writer::init() {
    if (is_initialized_) {
        return true;
    }
    
    // Set CPU affinity if requested
    if (config_.cpu_affinity >= 0) {
        CacheUtils::setCpuAffinity(config_.cpu_affinity);
    }
    
    // Calculate total size needed
    size_t aligned_buffer = CacheUtils::alignToCacheLine(max_size_);
    shm_size_ = sizeof(Header) + aligned_buffer * 2;
    
    // Try to use huge pages if beneficial
    if (config_.use_huge_pages && CacheUtils::shouldUseHugePages(shm_size_)) {
        shm_size_ = CacheUtils::alignToHugePage(shm_size_);
    }
    
    // Try to unlink existing
    shm_unlink(shm_name_.c_str());
    
    // Create shared memory
    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd_ == -1) {
        return false;
    }
    
    // Set size
    if (ftruncate(shm_fd_, shm_size_) == -1) {
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        return false;
    }
    
    // Map memory
    if (!allocateMemory()) {
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        return false;
    }
    
    // Initialize header
    header_ = static_cast<Header*>(shm_ptr_);
    std::memset(header_, 0, sizeof(Header));
    
    header_->magic = CASIR_MAGIC;
    header_->version = CASIR_VERSION;
    header_->capacity = max_size_;
    header_->huge_page_size = using_huge_pages_ ? HUGE_PAGE_SIZE : 0;
    header_->flags = using_huge_pages_ ? 1 : 0;
    
    header_->front_idx.store(0, std::memory_order_relaxed);
    header_->frame0.store(0, std::memory_order_relaxed);
    header_->frame1.store(0, std::memory_order_relaxed);
    header_->timestamp0_ns.store(0, std::memory_order_relaxed);
    header_->timestamp1_ns.store(0, std::memory_order_relaxed);
    header_->published_length.store(0, std::memory_order_relaxed);
    header_->checksum_enabled.store(false, std::memory_order_relaxed);
    header_->total_writes.store(0, std::memory_order_relaxed);
    header_->total_bytes.store(0, std::memory_order_relaxed);
    
    // Set buffer pointers
    size_t aligned_buffer_size = CacheUtils::alignToCacheLine(max_size_);
    uint8_t* base = static_cast<uint8_t*>(shm_ptr_) + sizeof(Header);
    buffer_[0] = base;
    buffer_[1] = base + aligned_buffer_size;
    
    // Prefetch buffers
    if (config_.enable_prefetch) {
        prefetchBuffer(0);
        prefetchBuffer(1);
    }
    
    is_initialized_ = true;
    return true;
}

bool Writer::allocateMemory() {
    int flags = MAP_SHARED | MAP_POPULATE;  // Pre-populate page tables
    
    // Try huge pages first
    if (config_.use_huge_pages) {
        auto hp_info = CacheUtils::detectHugePages();
        if (hp_info.usable) {
            void* ptr = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE,
                           flags | MAP_HUGETLB, shm_fd_, 0);
            if (ptr != MAP_FAILED) {
                shm_ptr_ = ptr;
                using_huge_pages_ = true;
                // Lock pages in RAM to prevent page faults
                mlock(shm_ptr_, shm_size_);
                return true;
            }
            // Fallback to regular pages
        }
    }
    
    // Regular mapping with MAP_POPULATE
    shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, flags, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        shm_ptr_ = nullptr;
        return false;
    }
    
    // Lock pages in RAM to prevent page faults during write
    mlock(shm_ptr_, shm_size_);
    
    // Advise kernel for sequential access and that we'll need this memory
    madvise(shm_ptr_, shm_size_, MADV_SEQUENTIAL);
    madvise(shm_ptr_, shm_size_, MADV_WILLNEED);
    
    using_huge_pages_ = false;
    return true;
}

void Writer::prefetchBuffer(int idx) {
    if (buffer_[idx] && config_.enable_prefetch) {
        CacheUtils::prefetchRange(buffer_[idx], 
            std::min(max_size_, config_.prefetch_distance));
    }
}

bool Writer::write(const void* data, size_t size) {
    if (!is_initialized_ || size > max_size_) {
        return false;
    }
    
    // Get back buffer index
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    uint32_t back = 1 - front;
    
    // Copy data to back buffer
    std::memcpy(buffer_[back], data, size);
    
    // Update metadata
    int64_t now = getCurrentTimestampNs();
    ++frame_count_;
    
    if (back == 0) {
        header_->frame0.store(frame_count_, std::memory_order_relaxed);
        header_->timestamp0_ns.store(now, std::memory_order_relaxed);
    } else {
        header_->frame1.store(frame_count_, std::memory_order_relaxed);
        header_->timestamp1_ns.store(now, std::memory_order_relaxed);
    }
    
    header_->published_length.store(size, std::memory_order_relaxed);
    header_->writer_heartbeat_ns.store(now, std::memory_order_relaxed);
    
    // Atomic swap - release ensures all writes are visible
    header_->front_idx.store(back, std::memory_order_release);
    
    return true;
}

bool Writer::writeZeroCopy(std::function<void(void*)> fill_func, size_t size) {
    if (!is_initialized_ || size > max_size_) {
        return false;
    }
    
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    uint32_t back = 1 - front;
    
    // Caller fills buffer directly
    fill_func(buffer_[back]);
    
    // Update and swap
    int64_t now = getCurrentTimestampNs();
    ++frame_count_;
    
    if (back == 0) {
        header_->frame0.store(frame_count_, std::memory_order_relaxed);
        header_->timestamp0_ns.store(now, std::memory_order_relaxed);
    } else {
        header_->frame1.store(frame_count_, std::memory_order_relaxed);
        header_->timestamp1_ns.store(now, std::memory_order_relaxed);
    }
    
    header_->published_length.store(size, std::memory_order_relaxed);
    header_->writer_heartbeat_ns.store(now, std::memory_order_relaxed);
    header_->front_idx.store(back, std::memory_order_release);
    
    header_->total_writes.fetch_add(1, std::memory_order_relaxed);
    header_->total_bytes.fetch_add(size, std::memory_order_relaxed);
    
    return true;
}

void* Writer::getWriteBuffer() {
    if (!is_initialized_) {
        return nullptr;
    }
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    return buffer_[1 - front];
}

bool Writer::commitWrite(size_t size) {
    if (!is_initialized_ || size > max_size_) {
        return false;
    }
    
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    uint32_t back = 1 - front;
    
    int64_t now = getCurrentTimestampNs();
    ++frame_count_;
    
    if (back == 0) {
        header_->frame0.store(frame_count_, std::memory_order_relaxed);
        header_->timestamp0_ns.store(now, std::memory_order_relaxed);
    } else {
        header_->frame1.store(frame_count_, std::memory_order_relaxed);
        header_->timestamp1_ns.store(now, std::memory_order_relaxed);
    }
    
    header_->published_length.store(size, std::memory_order_relaxed);
    header_->writer_heartbeat_ns.store(now, std::memory_order_relaxed);
    header_->front_idx.store(back, std::memory_order_release);
    
    header_->total_writes.fetch_add(1, std::memory_order_relaxed);
    header_->total_bytes.fetch_add(size, std::memory_order_relaxed);
    
    return true;
}

Stats Writer::getStats() const {
    Stats stats;
    stats.huge_pages_active = using_huge_pages_;
    stats.prefetch_active = config_.enable_prefetch;
    stats.numa_node = 0;
    stats.pinned_cpu = config_.cpu_affinity;
    stats.cache_info = cache_info_;
    stats.total_writes = header_ ? header_->total_writes.load() : 0;
    stats.total_reads = 0;
    stats.bytes_transferred = header_ ? header_->total_bytes.load() : 0;
    return stats;
}

void Writer::destroy() {
    if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, shm_size_);
        shm_ptr_ = nullptr;
    }
    
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        if (!shm_name_.empty()) {
            shm_unlink(shm_name_.c_str());
        }
        shm_fd_ = -1;
    }
    
    header_ = nullptr;
    buffer_[0] = nullptr;
    buffer_[1] = nullptr;
    is_initialized_ = false;
}

int64_t Writer::getCurrentTimestampNs() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

// ============================================================================
// Reader Implementation
// ============================================================================

Reader::Reader(const std::string& shm_name, size_t max_size,
                         const Config& config)
    : shm_name_(shm_name)
    , max_size_(max_size)
    , config_(config)
    , is_initialized_(false)
    , shm_fd_(-1)
    , shm_ptr_(nullptr)
    , shm_size_(0)
    , using_huge_pages_(false)
    , header_(nullptr)
    , last_frame_(0)
    , last_timestamp_ns_(0)
    , dropped_frames_(0)
    , last_checksum_valid_(true)
    , zero_copy_active_(false)
{
    buffer_[0] = nullptr;
    buffer_[1] = nullptr;
    cache_info_ = CacheUtils::detectCacheInfo();
    
    if (config_.prefetch_distance == 0) {
        config_.prefetch_distance = cache_info_.optimal_prefetch_distance();
    }
}

Reader::~Reader() {
    if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, shm_size_);
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
    }
}

Reader::Reader(Reader&& other) noexcept
    : shm_name_(std::move(other.shm_name_))
    , max_size_(other.max_size_)
    , config_(other.config_)
    , is_initialized_(other.is_initialized_)
    , shm_fd_(other.shm_fd_)
    , shm_ptr_(other.shm_ptr_)
    , shm_size_(other.shm_size_)
    , using_huge_pages_(other.using_huge_pages_)
    , header_(other.header_)
    , last_frame_(other.last_frame_)
    , last_timestamp_ns_(other.last_timestamp_ns_)
    , dropped_frames_(other.dropped_frames_)
    , last_checksum_valid_(other.last_checksum_valid_)
    , zero_copy_active_(other.zero_copy_active_)
    , cache_info_(other.cache_info_)
{
    buffer_[0] = other.buffer_[0];
    buffer_[1] = other.buffer_[1];
    
    other.is_initialized_ = false;
    other.shm_fd_ = -1;
    other.shm_ptr_ = nullptr;
    other.header_ = nullptr;
}

Reader& Reader::operator=(Reader&& other) noexcept {
    if (this != &other) {
        if (shm_ptr_) munmap(shm_ptr_, shm_size_);
        if (shm_fd_ >= 0) close(shm_fd_);
        
        shm_name_ = std::move(other.shm_name_);
        max_size_ = other.max_size_;
        config_ = other.config_;
        is_initialized_ = other.is_initialized_;
        shm_fd_ = other.shm_fd_;
        shm_ptr_ = other.shm_ptr_;
        shm_size_ = other.shm_size_;
        using_huge_pages_ = other.using_huge_pages_;
        header_ = other.header_;
        buffer_[0] = other.buffer_[0];
        buffer_[1] = other.buffer_[1];
        last_frame_ = other.last_frame_;
        last_timestamp_ns_ = other.last_timestamp_ns_;
        dropped_frames_ = other.dropped_frames_;
        last_checksum_valid_ = other.last_checksum_valid_;
        zero_copy_active_ = other.zero_copy_active_;
        cache_info_ = other.cache_info_;
        
        other.is_initialized_ = false;
        other.shm_fd_ = -1;
        other.shm_ptr_ = nullptr;
        other.header_ = nullptr;
    }
    return *this;
}

bool Reader::init() {
    if (is_initialized_) {
        return true;
    }
    
    // Set CPU affinity if requested
    if (config_.cpu_affinity >= 0) {
        CacheUtils::setCpuAffinity(config_.cpu_affinity);
    }
    
    // Open existing shared memory
    shm_fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0666);
    if (shm_fd_ == -1) {
        return false;
    }
    
    // Get size
    struct stat st;
    if (fstat(shm_fd_, &st) == -1) {
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }
    shm_size_ = st.st_size;
    
    // Map memory (try huge pages if available)
    int flags = MAP_SHARED;
    
    if (config_.use_huge_pages) {
        void* ptr = mmap(nullptr, shm_size_, PROT_READ, 
                        flags | MAP_HUGETLB, shm_fd_, 0);
        if (ptr != MAP_FAILED) {
            shm_ptr_ = ptr;
            using_huge_pages_ = true;
        }
    }
    
    if (!shm_ptr_) {
        shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ, flags, shm_fd_, 0);
        if (shm_ptr_ == MAP_FAILED) {
            close(shm_fd_);
            shm_fd_ = -1;
            shm_ptr_ = nullptr;
            return false;
        }
        using_huge_pages_ = false;
    }
    
    // Validate header
    header_ = static_cast<Header*>(shm_ptr_);
    if (header_->magic != CASIR_MAGIC) {
        munmap(shm_ptr_, shm_size_);
        close(shm_fd_);
        shm_ptr_ = nullptr;
        shm_fd_ = -1;
        return false;
    }
    
    // Set buffer pointers
    size_t aligned_buffer_size = CacheUtils::alignToCacheLine(max_size_);
    const uint8_t* base = static_cast<const uint8_t*>(shm_ptr_) + sizeof(Header);
    buffer_[0] = base;
    buffer_[1] = base + aligned_buffer_size;
    
    is_initialized_ = true;
    return true;
}

void Reader::prefetchBuffer(int idx) {
    if (buffer_[idx] && config_.enable_prefetch) {
        CacheUtils::prefetchRange(buffer_[idx], 
            std::min(max_size_, config_.prefetch_distance));
    }
}

bool Reader::read(void* data, size_t& size) {
    if (!is_initialized_) {
        return false;
    }
    
    // Load front index with acquire semantics
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    
    // Get current frame number
    uint64_t current_frame = (front == 0) ? 
        header_->frame0.load(std::memory_order_relaxed) :
        header_->frame1.load(std::memory_order_relaxed);
    
    // Check for new frame
    if (current_frame == last_frame_) {
        return false;  // No new data
    }
    
    // Track dropped frames
    if (last_frame_ > 0 && current_frame > last_frame_ + 1) {
        dropped_frames_ += (current_frame - last_frame_ - 1);
    }
    
    // Get size
    size_t data_size = header_->published_length.load(std::memory_order_relaxed);
    if (data_size > max_size_) {
        return false;
    }
    size = data_size;
    
    // Simple memcpy - CPU's built-in prefetch is already optimized
    std::memcpy(data, buffer_[front], data_size);
    
    // Update state
    last_frame_ = current_frame;
    last_timestamp_ns_ = (front == 0) ?
        header_->timestamp0_ns.load(std::memory_order_relaxed) :
        header_->timestamp1_ns.load(std::memory_order_relaxed);
    
    return true;
}

const void* Reader::readZeroCopy(size_t& size) {
    if (!is_initialized_ || zero_copy_active_) {
        return nullptr;
    }
    
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    
    uint64_t current_frame = (front == 0) ?
        header_->frame0.load(std::memory_order_relaxed) :
        header_->frame1.load(std::memory_order_relaxed);
    
    if (current_frame == last_frame_) {
        return nullptr;
    }
    
    size = header_->published_length.load(std::memory_order_relaxed);
    
    last_frame_ = current_frame;
    last_timestamp_ns_ = (front == 0) ?
        header_->timestamp0_ns.load(std::memory_order_relaxed) :
        header_->timestamp1_ns.load(std::memory_order_relaxed);
    
    zero_copy_active_ = true;
    return buffer_[front];
}

void Reader::releaseZeroCopy() {
    zero_copy_active_ = false;
}

bool Reader::readWithTimeout(void* data, size_t& size, uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        if (read(data, size)) {
            return true;
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        
        if (elapsed >= timeout_ms) {
            return false;
        }
        
        std::this_thread::yield();
    }
}

bool Reader::isWriterAlive(uint32_t timeout_ms) const {
    if (!is_initialized_) {
        return false;
    }
    
    int64_t heartbeat = header_->writer_heartbeat_ns.load(std::memory_order_relaxed);
    int64_t now = getCurrentTimestampNs();
    int64_t diff_ms = (now - heartbeat) / 1000000;
    
    return diff_ms < static_cast<int64_t>(timeout_ms);
}

Stats Reader::getStats() const {
    Stats stats;
    stats.huge_pages_active = using_huge_pages_;
    stats.prefetch_active = config_.enable_prefetch;
    stats.numa_node = 0;
    stats.pinned_cpu = config_.cpu_affinity;
    stats.cache_info = cache_info_;
    stats.total_writes = header_ ? header_->total_writes.load() : 0;
    stats.total_reads = last_frame_;
    stats.bytes_transferred = header_ ? header_->total_bytes.load() : 0;
    return stats;
}

int64_t Reader::getCurrentTimestampNs() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

} // namespace CASIR
