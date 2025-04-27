/**
 * @file main.cpp
 * @brief Main entry point for the QUIC proxy application
 * 
 * This program implements a QUIC proxy that accelerates satellite communications
 * by offloading heavy QUIC operations to FPGA hardware.
 */

#include <iostream>
#include <thread>
#include <csignal>
#include <cstring>
#include <vector>
#include <atomic>
#include <getopt.h>
#include "quic_proxy.h"
#include "fpga_interface.h"

// Global flag for graceful shutdown
std::atomic<bool> g_running(true);

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

// Print usage information
void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -p, --port PORT       Listen port for incoming connections (default: 8443)" << std::endl;
    std::cout << "  -b, --bind ADDRESS    Bind address (default: 0.0.0.0)" << std::endl;
    std::cout << "  -d, --device DEVICE   FPGA device path (default: /dev/fpga0)" << std::endl;
    std::cout << "  -a, --acceleration    Enable FPGA acceleration (default: enabled)" << std::endl;
    std::cout << "  -s, --simulation      Run in simulation mode without real FPGA" << std::endl;
    std::cout << "  -v, --verbose         Enable verbose logging" << std::endl;
    std::cout << "  -h, --help            Display this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    // Default configuration parameters
    int port = 8443;
    std::string bindAddress = "0.0.0.0";
    std::string devicePath = "/dev/fpga0";
    bool enableAcceleration = true;
    bool simulationMode = false;
    bool verboseLogging = false;
    
    // Parse command line arguments
    static struct option long_options[] = {
        {"port", required_argument, nullptr, 'p'},
        {"bind", required_argument, nullptr, 'b'},
        {"device", required_argument, nullptr, 'd'},
        {"acceleration", no_argument, nullptr, 'a'},
        {"simulation", no_argument, nullptr, 's'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "p:b:d:asvh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                port = std::atoi(optarg);
                break;
            case 'b':
                bindAddress = optarg;
                break;
            case 'd':
                devicePath = optarg;
                break;
            case 'a':
                enableAcceleration = true;
                break;
            case 's':
                simulationMode = true;
                break;
            case 'v':
                verboseLogging = true;
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            default:
                printUsage(argv[0]);
                return 1;
        }
    }
    
    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    try {
        std::cout << "Starting QUIC Proxy with FPGA acceleration" << std::endl;
        std::cout << "Binding to " << bindAddress << ":" << port << std::endl;
        
        // Initialize FPGA interface
        FPGAInterface fpgaInterface(devicePath, simulationMode);
        if (!fpgaInterface.initialize()) {
            std::cerr << "Failed to initialize FPGA interface" << std::endl;
            return 1;
        }
        
        // Create and configure QUIC proxy
        QUICProxy proxy(bindAddress, port, fpgaInterface);
        proxy.setAccelerationEnabled(enableAcceleration);
        proxy.setVerboseLogging(verboseLogging);
        
        // Start the proxy
        if (!proxy.start()) {
            std::cerr << "Failed to start QUIC proxy" << std::endl;
            return 1;
        }
        
        // Main loop - keep running until signal received
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Optional: print status periodically
            if (verboseLogging) {
                proxy.printStats();
            }
        }
        
        // Graceful shutdown
        std::cout << "Shutting down QUIC proxy..." << std::endl;
        proxy.stop();
        fpgaInterface.shutdown();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "QUIC proxy successfully shut down" << std::endl;
    return 0;
}
