/**
 * @file sim_transport.cpp
 * @brief Implementazione libreria SIM (Sensor-in-Memory)
 */

#include "sim.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace SIM {

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static size_t calculateShmSize(size_t max_payload) {
    // Header + 2 buffer (double buffering) + padding per allineamento
    return sizeof(Header) + (max_payload * 2) + 128;
}

static size_t alignToCacheLine(size_t size) {
    constexpr size_t CACHE_LINE = 64;
    return ((size + CACHE_LINE - 1) / CACHE_LINE) * CACHE_LINE;
}

// ============================================================================
// WRITER IMPLEMENTATION
// ============================================================================

Writer::Writer(const std::string& shm_name, size_t max_size, bool enable_checksum)
    : shm_name_(shm_name)
    , max_size_(max_size)
    , enable_checksum_(enable_checksum)
    , is_initialized_(false)
    , shm_fd_(-1)
    , shm_ptr_(nullptr)
    , shm_size_(0)
    , header_(nullptr)
    , frame_count_(0)
{
    buffer_[0] = nullptr;
    buffer_[1] = nullptr;
}

Writer::~Writer() {
    if (shm_ptr_ != nullptr && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, shm_size_);
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
    }
}

Writer::Writer(Writer&& other) noexcept
    : shm_name_(std::move(other.shm_name_))
    , max_size_(other.max_size_)
    , enable_checksum_(other.enable_checksum_)
    , is_initialized_(other.is_initialized_)
    , shm_fd_(other.shm_fd_)
    , shm_ptr_(other.shm_ptr_)
    , shm_size_(other.shm_size_)
    , header_(other.header_)
    , frame_count_(other.frame_count_)
{
    buffer_[0] = other.buffer_[0];
    buffer_[1] = other.buffer_[1];
    
    other.shm_fd_ = -1;
    other.shm_ptr_ = nullptr;
    other.header_ = nullptr;
    other.is_initialized_ = false;
}

Writer& Writer::operator=(Writer&& other) noexcept {
    if (this != &other) {
        // Cleanup current
        if (shm_ptr_ != nullptr && shm_ptr_ != MAP_FAILED) {
            munmap(shm_ptr_, shm_size_);
        }
        if (shm_fd_ >= 0) {
            close(shm_fd_);
        }
        
        // Move
        shm_name_ = std::move(other.shm_name_);
        max_size_ = other.max_size_;
        enable_checksum_ = other.enable_checksum_;
        is_initialized_ = other.is_initialized_;
        shm_fd_ = other.shm_fd_;
        shm_ptr_ = other.shm_ptr_;
        shm_size_ = other.shm_size_;
        header_ = other.header_;
        buffer_[0] = other.buffer_[0];
        buffer_[1] = other.buffer_[1];
        frame_count_ = other.frame_count_;
        
        other.shm_fd_ = -1;
        other.shm_ptr_ = nullptr;
        other.header_ = nullptr;
        other.is_initialized_ = false;
    }
    return *this;
}

bool Writer::init() {
    if (is_initialized_) {
        return true;
    }
    
    shm_size_ = calculateShmSize(max_size_);
    
    // Crea o apre shared memory
    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd_ < 0) {
        return false;
    }
    
    // Imposta dimensione
    if (ftruncate(shm_fd_, shm_size_) < 0) {
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }
    
    // Mappa in memoria (read-write)
    shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, 
                    MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        close(shm_fd_);
        shm_fd_ = -1;
        shm_ptr_ = nullptr;
        return false;
    }
    
    // Setup puntatori
    header_ = static_cast<Header*>(shm_ptr_);
    
    size_t header_size = alignToCacheLine(sizeof(Header));
    uint8_t* payload_start = static_cast<uint8_t*>(shm_ptr_) + header_size;
    buffer_[0] = payload_start;
    buffer_[1] = payload_start + max_size_;
    
    // Inizializza header
    header_->magic = HEADER_MAGIC;
    header_->version = (VERSION_MAJOR << 16) | VERSION_MINOR;
    header_->capacity = max_size_;
    header_->front_idx.store(0, std::memory_order_relaxed);
    header_->frame[0].store(0, std::memory_order_relaxed);
    header_->frame[1].store(0, std::memory_order_relaxed);
    header_->timestamp_ns[0].store(0, std::memory_order_relaxed);
    header_->timestamp_ns[1].store(0, std::memory_order_relaxed);
    header_->published_length.store(0, std::memory_order_relaxed);
    header_->writer_heartbeat_ns.store(getCurrentTimestampNs(), std::memory_order_relaxed);
    header_->checksum[0].store(CHECKSUM_DISABLED, std::memory_order_relaxed);
    header_->checksum[1].store(CHECKSUM_DISABLED, std::memory_order_relaxed);
    header_->checksum_enabled.store(enable_checksum_, std::memory_order_relaxed);
    
    // Memory barrier per assicurare visibilita
    std::atomic_thread_fence(std::memory_order_release);
    
    is_initialized_ = true;
    return true;
}

bool Writer::write(const void* data, size_t size) {
    if (!is_initialized_ || data == nullptr || size > max_size_) {
        return false;
    }
    
    // Seleziona back buffer (opposto al front)
    uint32_t front = header_->front_idx.load(std::memory_order_relaxed);
    uint32_t back = 1 - front;
    
    // Scrivi dati nel back buffer
    std::memcpy(buffer_[back], data, size);
    
    // Aggiorna metadati
    ++frame_count_;
    int64_t now = getCurrentTimestampNs();
    
    header_->frame[back].store(frame_count_, std::memory_order_relaxed);
    header_->timestamp_ns[back].store(now, std::memory_order_relaxed);
    header_->published_length.store(size, std::memory_order_relaxed);
    header_->writer_heartbeat_ns.store(now, std::memory_order_relaxed);
    
    // Calcola checksum se abilitato
    if (enable_checksum_) {
        uint32_t cs = calculateChecksum(data, size);
        header_->checksum[back].store(cs, std::memory_order_relaxed);
    }
    
    // Flip atomico del front_idx con release semantics
    // Questo garantisce che tutti gli store precedenti siano visibili
    header_->front_idx.store(back, std::memory_order_release);
    
    return true;
}

void Writer::setChecksumEnabled(bool enabled) {
    enable_checksum_ = enabled;
    if (header_ != nullptr) {
        header_->checksum_enabled.store(enabled, std::memory_order_relaxed);
    }
}

void Writer::destroy() {
    if (shm_ptr_ != nullptr && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, shm_size_);
        shm_ptr_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
    
    // Rimuovi shared memory
    shm_unlink(shm_name_.c_str());
    
    is_initialized_ = false;
    header_ = nullptr;
}

uint32_t Writer::calculateChecksum(const void* data, size_t size) const {
    // CRC32-like checksum semplice
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    
    return ~crc;
}

int64_t Writer::getCurrentTimestampNs() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return static_cast<int64_t>(ns);
}

// ============================================================================
// READER IMPLEMENTATION
// ============================================================================

Reader::Reader(const std::string& shm_name, size_t max_size)
    : shm_name_(shm_name)
    , max_size_(max_size)
    , is_initialized_(false)
    , shm_fd_(-1)
    , shm_ptr_(nullptr)
    , shm_size_(0)
    , header_(nullptr)
    , last_frame_(0)
    , last_timestamp_ns_(0)
    , dropped_frames_(0)
    , last_checksum_valid_(true)
{
    buffer_[0] = nullptr;
    buffer_[1] = nullptr;
}

Reader::~Reader() {
    if (shm_ptr_ != nullptr && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, shm_size_);
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
    }
}

Reader::Reader(Reader&& other) noexcept
    : shm_name_(std::move(other.shm_name_))
    , max_size_(other.max_size_)
    , is_initialized_(other.is_initialized_)
    , shm_fd_(other.shm_fd_)
    , shm_ptr_(other.shm_ptr_)
    , shm_size_(other.shm_size_)
    , header_(other.header_)
    , last_frame_(other.last_frame_)
    , last_timestamp_ns_(other.last_timestamp_ns_)
    , dropped_frames_(other.dropped_frames_)
    , last_checksum_valid_(other.last_checksum_valid_)
{
    buffer_[0] = other.buffer_[0];
    buffer_[1] = other.buffer_[1];
    
    other.shm_fd_ = -1;
    other.shm_ptr_ = nullptr;
    other.header_ = nullptr;
    other.is_initialized_ = false;
}

Reader& Reader::operator=(Reader&& other) noexcept {
    if (this != &other) {
        if (shm_ptr_ != nullptr && shm_ptr_ != MAP_FAILED) {
            munmap(shm_ptr_, shm_size_);
        }
        if (shm_fd_ >= 0) {
            close(shm_fd_);
        }
        
        shm_name_ = std::move(other.shm_name_);
        max_size_ = other.max_size_;
        is_initialized_ = other.is_initialized_;
        shm_fd_ = other.shm_fd_;
        shm_ptr_ = other.shm_ptr_;
        shm_size_ = other.shm_size_;
        header_ = other.header_;
        buffer_[0] = other.buffer_[0];
        buffer_[1] = other.buffer_[1];
        last_frame_ = other.last_frame_;
        last_timestamp_ns_ = other.last_timestamp_ns_;
        dropped_frames_ = other.dropped_frames_;
        last_checksum_valid_ = other.last_checksum_valid_;
        
        other.shm_fd_ = -1;
        other.shm_ptr_ = nullptr;
        other.header_ = nullptr;
        other.is_initialized_ = false;
    }
    return *this;
}

bool Reader::init() {
    if (is_initialized_) {
        return true;
    }
    
    shm_size_ = calculateShmSize(max_size_);
    
    // Apre shared memory esistente (read-only)
    shm_fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0666);
    if (shm_fd_ < 0) {
        return false;
    }
    
    // Mappa in memoria (read-only)
    shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        close(shm_fd_);
        shm_fd_ = -1;
        shm_ptr_ = nullptr;
        return false;
    }
    
    // Setup puntatori
    header_ = static_cast<Header*>(shm_ptr_);
    
    // Verifica magic number
    if (header_->magic != HEADER_MAGIC) {
        munmap(shm_ptr_, shm_size_);
        close(shm_fd_);
        shm_fd_ = -1;
        shm_ptr_ = nullptr;
        header_ = nullptr;
        return false;
    }
    
    size_t header_size = alignToCacheLine(sizeof(Header));
    const uint8_t* payload_start = static_cast<const uint8_t*>(shm_ptr_) + header_size;
    buffer_[0] = payload_start;
    buffer_[1] = payload_start + max_size_;
    
    is_initialized_ = true;
    return true;
}

bool Reader::read(void* data, size_t& size) {
    if (!is_initialized_ || data == nullptr) {
        return false;
    }
    
    // Leggi front_idx con acquire semantics
    uint32_t front = header_->front_idx.load(std::memory_order_acquire);
    
    // Controlla se c'e un nuovo frame
    uint64_t current_frame = header_->frame[front].load(std::memory_order_relaxed);
    
    if (current_frame == last_frame_) {
        // Nessun nuovo frame
        return false;
    }
    
    // Calcola frame persi
    if (current_frame > last_frame_ + 1) {
        dropped_frames_ += (current_frame - last_frame_ - 1);
    }
    
    // Leggi dimensione e timestamp
    size_t published_len = header_->published_length.load(std::memory_order_relaxed);
    last_timestamp_ns_ = header_->timestamp_ns[front].load(std::memory_order_relaxed);
    
    // Copia dati
    if (published_len > max_size_) {
        published_len = max_size_;
    }
    std::memcpy(data, buffer_[front], published_len);
    size = published_len;
    
    // Verifica checksum se abilitato
    if (header_->checksum_enabled.load(std::memory_order_relaxed)) {
        uint32_t expected = header_->checksum[front].load(std::memory_order_relaxed);
        if (expected != CHECKSUM_DISABLED) {
            uint32_t actual = calculateChecksum(data, size);
            last_checksum_valid_ = (actual == expected);
        } else {
            last_checksum_valid_ = true;
        }
    } else {
        last_checksum_valid_ = true;
    }
    
    // Aggiorna stato
    last_frame_ = current_frame;
    
    return true;
}

bool Reader::readWithTimeout(void* data, size_t& size, uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (read(data, size)) {
            return true;
        }
        // Breve sleep per non saturare CPU
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    return false;
}

bool Reader::isWriterAlive(uint32_t timeout_ms) const {
    if (!is_initialized_ || header_ == nullptr) {
        return false;
    }
    
    int64_t last_heartbeat = header_->writer_heartbeat_ns.load(std::memory_order_relaxed);
    int64_t now = getCurrentTimestampNs();
    int64_t elapsed_ms = (now - last_heartbeat) / 1000000;
    
    return elapsed_ms < static_cast<int64_t>(timeout_ms);
}

uint32_t Reader::calculateChecksum(const void* data, size_t size) const {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    
    return ~crc;
}

int64_t Reader::getCurrentTimestampNs() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return static_cast<int64_t>(ns);
}

// ============================================================================
// C-STYLE API
// ============================================================================

namespace C_API {

static std::unique_ptr<Writer> g_writer;
static std::unique_ptr<Reader> g_reader;

bool writerInit(const char* shm_name, size_t size, bool enable_checksum) {
    g_writer = std::make_unique<Writer>(shm_name, size, enable_checksum);
    return g_writer->init();
}

bool readerInit(const char* shm_name, size_t size) {
    g_reader = std::make_unique<Reader>(shm_name, size);
    return g_reader->init();
}

bool writeData(const char* shm_name, const void* data, size_t size) {
    if (!g_writer || g_writer->getName() != shm_name) {
        return false;
    }
    return g_writer->write(data, size);
}

bool readData(const char* shm_name, void* data, size_t* size) {
    (void)shm_name;  // Suppress unused parameter warning
    if (!g_reader || !size) {
        return false;
    }
    return g_reader->read(data, *size);
}

void destroy(const char* shm_name) {
    if (g_writer && g_writer->getName() == shm_name) {
        g_writer->destroy();
        g_writer.reset();
    }
    if (g_reader) {
        g_reader.reset();
    }
    // Anche se non abbiamo riferimenti, prova a rimuovere
    shm_unlink(shm_name);
}

bool isAvailable(const char* shm_name) {
    int fd = shm_open(shm_name, O_RDONLY, 0666);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

} // namespace C_API

} // namespace SIM
