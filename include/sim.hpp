/**
 * @file sim_transport.hpp
 * @brief SIM (Sensor-in-Memory) Transport Library
 * 
 * Libreria di trasporto shared-memory lock-free per applicazioni
 * real-time come veicoli autonomi. Basata sul paper:
 * "A Faster and More Reliable Middleware for Autonomous Driving Systems"
 * 
 * Caratteristiche:
 * - Double buffering lock-free
 * - Nessuna serializzazione
 * - Latenza sub-millisecondo
 * - Checksum opzionale
 * - POSIX shared memory
 * 
 * Uso tipico:
 *   // Writer
 *   SIM::Writer writer("/my_sensor", 1920*1080*3);
 *   writer.write(data, size);
 * 
 *   // Reader
 *   SIM::Reader reader("/my_sensor", 1920*1080*3);
 *   reader.read(buffer, size);
 */

#ifndef SIM_TRANSPORT_HPP
#define SIM_TRANSPORT_HPP

#include <string>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <memory>

namespace SIM {

// Versione libreria
constexpr uint32_t VERSION_MAJOR = 1;
constexpr uint32_t VERSION_MINOR = 0;

// Configurazione default
constexpr size_t DEFAULT_MAX_SIZE = 1920 * 1080 * 3;  // 1080p RGB
constexpr uint32_t CHECKSUM_DISABLED = 0xFFFFFFFF;

/**
 * @struct Header
 * @brief Header della regione shared memory
 * 
 * Contiene metadati per sincronizzazione lock-free tra writer e reader.
 */
struct Header {
    // Magic number per validazione
    uint32_t magic;
    
    // Versione protocollo
    uint32_t version;
    
    // Capacita massima buffer
    size_t capacity;
    
    // Indice buffer attivo (0 o 1) - atomico
    std::atomic<uint32_t> front_idx;
    
    // Frame number per ogni buffer
    std::atomic<uint64_t> frame[2];
    
    // Timestamp (nanosecondi) per ogni buffer
    std::atomic<int64_t> timestamp_ns[2];
    
    // Dimensione dati attualmente pubblicati
    std::atomic<size_t> published_length;
    
    // Heartbeat writer (ultimo timestamp scrittura)
    std::atomic<int64_t> writer_heartbeat_ns;
    
    // Checksum per ogni buffer (0xFFFFFFFF = disabilitato)
    std::atomic<uint32_t> checksum[2];
    
    // Flag checksum abilitato
    std::atomic<bool> checksum_enabled;
    
    // Padding per allineamento cache line (64 byte)
    char padding[16];
};

// Magic number per validazione header
constexpr uint32_t HEADER_MAGIC = 0x53494D00;  // "SIM\0"

/**
 * @class Writer
 * @brief Scrittore SIM per pubblicare dati in shared memory
 */
class Writer {
public:
    /**
     * @brief Costruttore
     * @param shm_name Nome shared memory (es. "/camera_front")
     * @param max_size Dimensione massima payload in byte
     * @param enable_checksum Abilita verifica checksum (default: false)
     */
    Writer(const std::string& shm_name, size_t max_size, bool enable_checksum = false);
    
    /**
     * @brief Distruttore - rilascia risorse ma NON cancella shared memory
     */
    ~Writer();
    
    // Non copiabile
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    
    // Movibile
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;
    
    /**
     * @brief Inizializza la regione shared memory
     * @return true se successo, false altrimenti
     */
    bool init();
    
    /**
     * @brief Scrive dati nel buffer
     * @param data Puntatore ai dati
     * @param size Dimensione dati in byte
     * @return true se successo, false se size > capacity
     */
    bool write(const void* data, size_t size);
    
    /**
     * @brief Verifica se la shared memory e pronta
     */
    bool isReady() const { return is_initialized_; }
    
    /**
     * @brief Ottiene il nome della shared memory
     */
    const std::string& getName() const { return shm_name_; }
    
    /**
     * @brief Ottiene la capacita massima
     */
    size_t getCapacity() const { return max_size_; }
    
    /**
     * @brief Ottiene il numero di frame scritti
     */
    uint64_t getFrameCount() const { return frame_count_; }
    
    /**
     * @brief Abilita/disabilita checksum a runtime
     */
    void setChecksumEnabled(bool enabled);
    
    /**
     * @brief Distrugge la shared memory (da chiamare solo quando finito)
     */
    void destroy();

private:
    std::string shm_name_;
    size_t max_size_;
    bool enable_checksum_;
    bool is_initialized_;
    int shm_fd_;
    void* shm_ptr_;
    size_t shm_size_;
    Header* header_;
    uint8_t* buffer_[2];
    uint64_t frame_count_;
    
    uint32_t calculateChecksum(const void* data, size_t size) const;
    int64_t getCurrentTimestampNs() const;
};

/**
 * @class Reader
 * @brief Lettore SIM per consumare dati da shared memory
 */
class Reader {
public:
    /**
     * @brief Costruttore
     * @param shm_name Nome shared memory (es. "/camera_front")
     * @param max_size Dimensione massima payload in byte
     */
    Reader(const std::string& shm_name, size_t max_size);
    
    /**
     * @brief Distruttore - rilascia mappatura ma NON cancella shared memory
     */
    ~Reader();
    
    // Non copiabile
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    
    // Movibile
    Reader(Reader&& other) noexcept;
    Reader& operator=(Reader&& other) noexcept;
    
    /**
     * @brief Inizializza connessione alla shared memory
     * @return true se successo, false altrimenti
     */
    bool init();
    
    /**
     * @brief Legge dati dal buffer se disponibile nuovo frame
     * @param data Buffer destinazione (deve essere >= capacity)
     * @param size Output: dimensione dati letti
     * @return true se nuovo frame letto, false se nessun nuovo frame
     */
    bool read(void* data, size_t& size);
    
    /**
     * @brief Legge dati con timeout
     * @param data Buffer destinazione
     * @param size Output: dimensione dati letti
     * @param timeout_ms Timeout in millisecondi
     * @return true se nuovo frame letto entro timeout
     */
    bool readWithTimeout(void* data, size_t& size, uint32_t timeout_ms);
    
    /**
     * @brief Verifica se la shared memory e pronta
     */
    bool isReady() const { return is_initialized_; }
    
    /**
     * @brief Verifica se il writer e attivo (heartbeat recente)
     * @param timeout_ms Timeout heartbeat in millisecondi
     */
    bool isWriterAlive(uint32_t timeout_ms = 1000) const;
    
    /**
     * @brief Ottiene timestamp dell'ultimo frame letto
     */
    int64_t getLastTimestampNs() const { return last_timestamp_ns_; }
    
    /**
     * @brief Ottiene numero frame dell'ultimo letto
     */
    uint64_t getLastFrameNumber() const { return last_frame_; }
    
    /**
     * @brief Ottiene numero di frame persi (skipped)
     */
    uint64_t getDroppedFrames() const { return dropped_frames_; }
    
    /**
     * @brief Verifica checksum dell'ultimo frame (se abilitato)
     * @return true se checksum OK o disabilitato, false se corrotto
     */
    bool verifyLastChecksum() const { return last_checksum_valid_; }

private:
    std::string shm_name_;
    size_t max_size_;
    bool is_initialized_;
    int shm_fd_;
    void* shm_ptr_;
    size_t shm_size_;
    Header* header_;
    const uint8_t* buffer_[2];
    uint64_t last_frame_;
    int64_t last_timestamp_ns_;
    uint64_t dropped_frames_;
    bool last_checksum_valid_;
    
    uint32_t calculateChecksum(const void* data, size_t size) const;
    int64_t getCurrentTimestampNs() const;
};

/**
 * @brief Funzioni helper per compatibilita con API C-style
 */
namespace C_API {
    bool writerInit(const char* shm_name, size_t size, bool enable_checksum = false);
    bool readerInit(const char* shm_name, size_t size);
    bool writeData(const char* shm_name, const void* data, size_t size);
    bool readData(const char* shm_name, void* data, size_t* size);
    void destroy(const char* shm_name);
    bool isAvailable(const char* shm_name);
}

} // namespace SIM

#endif // SIM_TRANSPORT_HPP
