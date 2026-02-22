/**
 * @file turbo_writer.cpp
 * @brief SIM Turbo - Writer Example with 5MB Data
 * 
 * Demonstrates SIM Turbo with cache-optimized writes.
 * 
 * Compile:
 *   g++ -std=c++17 turbo_writer.cpp ../src/sim_turbo.cpp ../src/cache_utils.cpp \
 *       -I../include -lrt -lpthread -O3 -march=native -o turbo_writer
 * 
 * Run:
 *   ./turbo_writer
 */

#include "sim_turbo.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <vector>

// Configuration
const std::string SHM_NAME = "/turbo_sensor";
const size_t DATA_SIZE = 5 * 1024 * 1024;  // 5 MB

volatile bool running = true;

void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    
    std::cout << "=== SIM Turbo Writer Example ===" << std::endl;
    std::cout << "Channel: " << SHM_NAME << std::endl;
    std::cout << "Size: " << DATA_SIZE / (1024*1024) << " MB" << std::endl;
    std::cout << std::endl;
    
    // Auto-detect optimal configuration
    auto config = SIM::TurboConfig::autoDetect();
    std::cout << "[Turbo] Config: huge_pages=" << (config.use_huge_pages ? "yes" : "no")
              << ", prefetch=" << config.prefetch_distance << std::endl;
    
    // Create Turbo writer
    SIM::TurboWriter writer(SHM_NAME, DATA_SIZE, config);
    
    if (!writer.init()) {
        std::cerr << "[Turbo] Failed to initialize!" << std::endl;
        return 1;
    }
    
    std::cout << "[Turbo] Initialized. Publishing at 30 Hz..." << std::endl;
    std::cout << "[Turbo] Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;
    
    // Prepare data buffer
    std::vector<uint8_t> data(DATA_SIZE);
    uint64_t sequence = 0;
    
    // Fill with pattern
    for (size_t i = 0; i < DATA_SIZE; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_log_seq = 0;
    auto last_log_time = start_time;
    double total_latency = 0;
    uint64_t latency_count = 0;
    
    while (running) {
        // Update sequence in data
        std::memcpy(data.data(), &sequence, sizeof(sequence));
        
        // Measure write time
        auto t1 = std::chrono::high_resolution_clock::now();
        
        if (!writer.write(data.data(), DATA_SIZE)) {
            std::cerr << "[Turbo] Write failed!" << std::endl;
        }
        
        auto t2 = std::chrono::high_resolution_clock::now();
        double write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        total_latency += write_ms;
        ++latency_count;
        
        // Log every second
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
            uint64_t fps = sequence - last_log_seq;
            double avg_latency = total_latency / latency_count;
            
            std::cout << "[Turbo] Seq: " << sequence 
                      << " | FPS: " << fps
                      << " | Avg write: " << std::fixed << std::setprecision(3) 
                      << avg_latency << " ms" << std::endl;
            
            last_log_seq = sequence;
            last_log_time = now;
            total_latency = 0;
            latency_count = 0;
        }
        
        ++sequence;
        
        // 30 Hz
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    
    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    
    std::cout << std::endl;
    std::cout << "[Turbo] Statistics:" << std::endl;
    std::cout << "  Total frames: " << sequence << std::endl;
    std::cout << "  Duration: " << std::fixed << std::setprecision(1) << elapsed << " s" << std::endl;
    std::cout << "  Average FPS: " << std::setprecision(1) << (sequence / elapsed) << std::endl;
    
    auto stats = writer.getStats();
    std::cout << "  Total bytes: " << (stats.total_bytes / (1024*1024)) << " MB" << std::endl;
    
    writer.destroy();
    std::cout << "[Turbo] Done." << std::endl;
    
    return 0;
}
