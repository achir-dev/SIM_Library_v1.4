/**
 * @file turbo_reader.cpp
 * @brief SIM Turbo - Reader Example with 5MB Data
 * 
 * Demonstrates SIM Turbo with cache-optimized zero-copy reads.
 * 
 * Compile:
 *   g++ -std=c++17 turbo_reader.cpp ../src/sim_turbo.cpp ../src/cache_utils.cpp \
 *       -I../include -lrt -lpthread -O3 -march=native -o turbo_reader
 * 
 * Run (start this BEFORE writer):
 *   ./turbo_reader
 */

#include "sim_turbo.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <iomanip>

// Configuration (must match writer)
const std::string SHM_NAME = "/turbo_sensor";
const size_t DATA_SIZE = 5 * 1024 * 1024;  // 5 MB

volatile bool running = true;

void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    
    std::cout << "=== SIM Turbo Reader Example ===" << std::endl;
    std::cout << "Channel: " << SHM_NAME << std::endl;
    std::cout << "Size: " << DATA_SIZE / (1024*1024) << " MB" << std::endl;
    std::cout << std::endl;
    
    // Auto-detect optimal configuration
    auto config = SIM::TurboConfig::autoDetect();
    std::cout << "[Turbo] Config: huge_pages=" << (config.use_huge_pages ? "yes" : "no")
              << ", prefetch=" << config.prefetch_distance << std::endl;
    
    // Create Turbo reader
    SIM::TurboReader reader(SHM_NAME, DATA_SIZE, config);
    
    std::cout << "[Turbo] Waiting for writer..." << std::endl;
    
    // Wait for writer
    while (!reader.init() && running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!running) {
        std::cout << "[Turbo] Cancelled." << std::endl;
        return 0;
    }
    
    std::cout << "[Turbo] Connected!" << std::endl;
    std::cout << "[Turbo] Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;
    
    uint64_t frame_count = 0;
    double total_latency = 0;
    uint64_t latency_samples = 0;
    auto last_log = std::chrono::steady_clock::now();
    
    while (running) {
        size_t size = 0;
        
        // Zero-copy read
        const void* data = reader.readZeroCopy(size);
        
        if (data && size > 0) {
            ++frame_count;
            
            // Calculate latency
            int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count();
            int64_t msg_ns = reader.getLastTimestampNs();
            double latency_ms = static_cast<double>(now_ns - msg_ns) / 1e6;
            
            total_latency += latency_ms;
            ++latency_samples;
            
            // Extract sequence (first 8 bytes)
            uint64_t sequence = 0;
            std::memcpy(&sequence, data, sizeof(sequence));
            
            // Release zero-copy buffer
            reader.releaseZeroCopy();
            
            // Log every second
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 1) {
                double avg_latency = total_latency / latency_samples;
                
                std::cout << "[Turbo] Seq: " << sequence 
                          << " | Latency: " << std::fixed << std::setprecision(3) << avg_latency << " ms"
                          << " | Dropped: " << reader.getDroppedFrames()
                          << " | Checksum: " << (reader.verifyLastChecksum() ? "OK" : "FAIL")
                          << std::endl;
                
                total_latency = 0;
                latency_samples = 0;
                last_log = now;
            }
        }
        
        // Check writer alive
        if (!reader.isWriterAlive(2000)) {
            std::cout << "[Turbo] WARNING: Writer timeout" << std::endl;
        }
        
        std::this_thread::yield();
    }
    
    std::cout << std::endl;
    std::cout << "[Turbo] Statistics:" << std::endl;
    std::cout << "  Total frames: " << frame_count << std::endl;
    std::cout << "  Dropped: " << reader.getDroppedFrames() << std::endl;
    
    auto stats = reader.getStats();
    std::cout << "  Total reads: " << stats.total_reads << std::endl;
    
    std::cout << "[Turbo] Done." << std::endl;
    
    return 0;
}
