/**
 * @file simple_reader.cpp
 * @brief SIM Library - Simple Reader Example
 * 
 * Demonstrates basic SIM::Reader usage with latency measurement.
 * 
 * Compile:
 *   g++ -std=c++17 simple_reader.cpp ../src/sim_transport.cpp \
 *       -I../include -lrt -lpthread -o simple_reader
 * 
 * Run:
 *   ./simple_reader
 */

#include "sim_transport.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <iomanip>

// Configuration (must match writer)
const std::string SHM_NAME = "/sensor_data";
const size_t DATA_SIZE = 1024;  // 1 KB

volatile bool running = true;

void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    
    std::cout << "=== SIM Reader Example ===" << std::endl;
    std::cout << "Channel: " << SHM_NAME << std::endl;
    std::cout << "Size: " << DATA_SIZE << " bytes" << std::endl;
    std::cout << std::endl;
    
    // Create reader
    SIM::Reader reader(SHM_NAME, DATA_SIZE);
    
    std::cout << "[Reader] Waiting for writer..." << std::endl;
    
    // Wait for writer
    while (!reader.init() && running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!running) {
        std::cout << "[Reader] Cancelled." << std::endl;
        return 0;
    }
    
    std::cout << "[Reader] Connected!" << std::endl;
    std::cout << "[Reader] Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;
    
    // Buffer
    std::vector<uint8_t> buffer(DATA_SIZE);
    
    uint64_t frame_count = 0;
    auto last_log = std::chrono::steady_clock::now();
    
    while (running) {
        size_t size = 0;
        
        if (reader.read(buffer.data(), size)) {
            ++frame_count;
            
            // Extract sequence from data
            uint64_t sequence = 0;
            std::memcpy(&sequence, buffer.data(), sizeof(sequence));
            
            // Log every second
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_log).count();
            
            if (elapsed >= 1) {
                std::cout << "[Reader] Seq: " << sequence 
                          << " | Dropped: " << reader.getDroppedFrames()
                          << std::endl;
                last_log = now;
            }
        }
        
        // Check writer alive
        if (!reader.isWriterAlive(2000)) {
            std::cout << "[Reader] WARNING: Writer timeout" << std::endl;
        }
        
        std::this_thread::yield();
    }
    
    std::cout << std::endl;
    std::cout << "[Reader] Total frames: " << frame_count << std::endl;
    std::cout << "[Reader] Dropped: " << reader.getDroppedFrames() << std::endl;
    std::cout << "[Reader] Done." << std::endl;
    
    return 0;
}
