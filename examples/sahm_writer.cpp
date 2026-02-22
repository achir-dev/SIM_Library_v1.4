/**
 * @file sahm_writer.cpp
 * @brief SAHM Library - Writer Example
 * 
 * Demonstrates SAHM::DirectWriter with ring buffer architecture.
 * SAHM = Sensor Acquisition to Host Memory
 * 
 * Compile:
 *   g++ -std=c++17 sahm_writer.cpp ../src/sahm.cpp \
 *       -I../include -lrt -lpthread -o sahm_writer
 * 
 * Run (start reader first!):
 *   ./sahm_writer
 */

#include "sahm.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>

// Configuration
const std::string CHANNEL = "/sensor_channel";
const size_t DATA_SIZE = 1024;  // 1 KB per slot

volatile bool running = true;

void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    
    std::cout << "=== SAHM Writer Example ===" << std::endl;
    std::cout << "Channel: " << CHANNEL << std::endl;
    std::cout << "Slot size: " << DATA_SIZE << " bytes" << std::endl;
    std::cout << std::endl;
    
    // Create SAHM writer
    SAHM::DirectWriter writer(CHANNEL, DATA_SIZE);
    
    if (!writer.init()) {
        std::cerr << "Failed to initialize SAHM writer!" << std::endl;
        return 1;
    }
    
    std::cout << "[Writer] Initialized. Waiting for readers..." << std::endl;
    
    // Wait for at least one reader
    while (writer.getReaderCount() == 0 && running) {
        std::cout << "[Writer] Waiting for readers..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    if (!running) {
        writer.destroy();
        return 0;
    }
    
    std::cout << "[Writer] " << writer.getReaderCount() << " reader(s) connected!" << std::endl;
    std::cout << "[Writer] Publishing at 30 Hz. Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;
    
    // Data buffer
    std::vector<uint8_t> data(DATA_SIZE);
    uint64_t sequence = 0;
    
    while (running) {
        // Fill data
        std::memset(data.data(), 0, DATA_SIZE);
        std::memcpy(data.data(), &sequence, sizeof(sequence));
        
        // Sample data
        for (size_t i = sizeof(sequence); i < DATA_SIZE; ++i) {
            data[i] = static_cast<uint8_t>((sequence + i) & 0xFF);
        }
        
        // Write to all readers
        int readers_written = writer.write(data.data(), DATA_SIZE);
        
        // Log every second
        if (sequence % 30 == 0) {
            std::cout << "[Writer] Seq: " << sequence 
                      << " | Readers: " << readers_written << std::endl;
        }
        
        ++sequence;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));  // 30 Hz
    }
    
    std::cout << std::endl;
    std::cout << "[Writer] Total: " << sequence << " frames" << std::endl;
    
    writer.destroy();
    std::cout << "[Writer] Done." << std::endl;
    
    return 0;
}
