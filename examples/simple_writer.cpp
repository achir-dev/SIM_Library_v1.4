/**
 * @file simple_writer.cpp
 * @brief SIM Library - Simple Writer Example
 * 
 * Demonstrates basic SIM::Writer usage with simple data.
 * 
 * Compile:
 *   g++ -std=c++17 simple_writer.cpp ../src/sim_transport.cpp \
 *       -I../include -lrt -lpthread -o simple_writer
 * 
 * Run:
 *   ./simple_writer
 */

#include "sim_transport.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>

// Configuration
const std::string SHM_NAME = "/sensor_data";
const size_t DATA_SIZE = 1024;  // 1 KB

volatile bool running = true;

void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    
    std::cout << "=== SIM Writer Example ===" << std::endl;
    std::cout << "Channel: " << SHM_NAME << std::endl;
    std::cout << "Size: " << DATA_SIZE << " bytes" << std::endl;
    std::cout << std::endl;
    
    // Create writer
    SIM::Writer writer(SHM_NAME, DATA_SIZE, false);
    
    if (!writer.init()) {
        std::cerr << "Failed to initialize writer!" << std::endl;
        return 1;
    }
    
    std::cout << "[Writer] Ready. Publishing at 30 Hz..." << std::endl;
    std::cout << "[Writer] Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;
    
    // Prepare data buffer
    std::vector<uint8_t> data(DATA_SIZE);
    uint64_t sequence = 0;
    
    while (running) {
        // Fill data with sequence number and sample values
        std::memset(data.data(), 0, DATA_SIZE);
        std::memcpy(data.data(), &sequence, sizeof(sequence));
        
        // Random-ish data for demo
        for (size_t i = sizeof(sequence); i < DATA_SIZE; ++i) {
            data[i] = static_cast<uint8_t>((sequence + i) & 0xFF);
        }
        
        // Write to SHM
        if (!writer.write(data.data(), DATA_SIZE)) {
            std::cerr << "[Writer] Write failed!" << std::endl;
        }
        
        // Log every 30 frames (1 second)
        if (sequence % 30 == 0) {
            std::cout << "[Writer] Sequence: " << sequence << std::endl;
        }
        
        ++sequence;
        
        // 30 Hz
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    
    std::cout << std::endl;
    std::cout << "[Writer] Stopping..." << std::endl;
    std::cout << "[Writer] Total frames: " << sequence << std::endl;
    
    // Cleanup
    writer.destroy();
    
    std::cout << "[Writer] Done." << std::endl;
    return 0;
}
