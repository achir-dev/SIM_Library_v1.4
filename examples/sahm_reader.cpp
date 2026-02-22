/**
 * @file sahm_reader.cpp
 * @brief SAHM Library - Reader Example
 * 
 * Demonstrates SAHM::DirectReader with ring buffer and zero-copy access.
 * SAHM = Sensor Acquisition to Host Memory
 * 
 * Compile:
 *   g++ -std=c++17 sahm_reader.cpp ../src/sahm.cpp \
 *       -I../include -lrt -lpthread -o sahm_reader
 * 
 * Run (start this BEFORE writer!):
 *   ./sahm_reader
 */

#include "sahm.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>

// Configuration (must match writer)
const std::string CHANNEL = "/sensor_channel";
const size_t DATA_SIZE = 1024;     // 1 KB per slot
const uint32_t RING_SIZE = 30;     // 30 slots = 1 second at 30 Hz

volatile bool running = true;

void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    
    std::cout << "=== SAHM Reader Example ===" << std::endl;
    std::cout << "Channel: " << CHANNEL << std::endl;
    std::cout << "Slot size: " << DATA_SIZE << " bytes" << std::endl;
    std::cout << "Ring size: " << RING_SIZE << " slots" << std::endl;
    std::cout << std::endl;
    
    // Create SAHM reader with ring buffer
    SAHM::DirectReader reader(CHANNEL, DATA_SIZE, RING_SIZE);
    
    std::cout << "[Reader] Waiting for writer..." << std::endl;
    
    // Wait for writer
    while (!reader.init() && running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!running) {
        return 0;
    }
    
    std::cout << "[Reader] Connected to SAHM channel!" << std::endl;
    std::cout << "[Reader] Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;
    
    uint64_t last_total = 0;
    auto last_log = std::chrono::steady_clock::now();
    
    while (running) {
        uint64_t total = reader.getTotalWrites();
        
        if (total > last_total) {
            // New data available - read via zero-copy pointer!
            size_t size;
            const void* data = reader.getLatest(size);
            
            if (data && size >= sizeof(uint64_t)) {
                // Extract sequence from data (zero-copy!)
                uint64_t sequence;
                std::memcpy(&sequence, data, sizeof(sequence));
                
                // Log every second
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_log).count() >= 1) {
                    
                    int64_t ts = reader.getLatestTimestampNs();
                    std::cout << "[Reader] Seq: " << sequence 
                              << " | Total: " << total
                              << " | Slot: " << reader.getWriteIndex() << std::endl;
                    last_log = now;
                }
            }
            
            last_total = total;
        }
        
        // Check writer alive
        if (!reader.isWriterAlive(2000)) {
            std::cout << "[Reader] WARNING: Writer timeout" << std::endl;
        }
        
        std::this_thread::yield();
    }
    
    std::cout << std::endl;
    std::cout << "[Reader] Total writes received: " << reader.getTotalWrites() << std::endl;
    std::cout << "[Reader] Done." << std::endl;
    
    return 0;
}
