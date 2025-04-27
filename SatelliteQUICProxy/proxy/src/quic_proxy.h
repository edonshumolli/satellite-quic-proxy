/**
 * @file quic_proxy.h
 * @brief QUIC proxy class for accelerating QUIC protocol over satellite links
 * 
 * The QUIC proxy accelerates QUIC communications over high-latency satellite links
 * by offloading heavy operations to FPGA hardware and optimizing for satellite 
 * channel characteristics.
 */

#ifndef QUIC_PROXY_H
#define QUIC_PROXY_H

#include <string>
#include <memory>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <map>
#include "quic_handler.h"
#include "fpga_interface.h"

/**
 * @class QUICProxy
 * @brief Main proxy class that manages QUIC connections and FPGA acceleration
 */
class QUICProxy {
public:
    /**
     * Constructor for the QUIC proxy
     * 
     * @param bindAddress The address to bind the proxy server to
     * @param port The port to listen on
     * @param fpgaInterface Reference to the FPGA interface for hardware acceleration
     */
    QUICProxy(const std::string& bindAddress, int port, FPGAInterface& fpgaInterface);
    
    /**
     * Destructor ensures proper cleanup
     */
    ~QUICProxy();
    
    /**
     * Start the QUIC proxy server
     * 
     * @return true if started successfully, false otherwise
     */
    bool start();
    
    /**
     * Stop the QUIC proxy server and cleanup resources
     */
    void stop();
    
    /**
     * Enable or disable FPGA acceleration
     * 
     * @param enabled Set to true to enable acceleration, false for software-only mode
     */
    void setAccelerationEnabled(bool enabled);
    
    /**
     * Enable or disable verbose logging
     * 
     * @param verbose Set to true for detailed logging
     */
    void setVerboseLogging(bool verbose);
    
    /**
     * Print current proxy statistics
     */
    void printStats() const;

private:
    // Configuration
    std::string m_bindAddress;
    int m_port;
    FPGAInterface& m_fpgaInterface;
    bool m_accelerationEnabled;
    bool m_verboseLogging;
    
    // Server state
    std::atomic<bool> m_running;
    std::unique_ptr<std::thread> m_acceptThread;
    int m_serverSocket;
    
    // Connection management
    std::vector<std::unique_ptr<QUICHandler>> m_connections;
    std::mutex m_connectionsMutex;
    
    // Statistics
    std::atomic<uint64_t> m_totalBytesReceived;
    std::atomic<uint64_t> m_totalBytesSent;
    std::atomic<uint64_t> m_totalPacketsReceived;
    std::atomic<uint64_t> m_totalPacketsSent;
    std::atomic<uint64_t> m_totalConnections;
    std::atomic<uint64_t> m_activeConnections;
    
    // Private helper methods
    void acceptLoop();
    void cleanupInactiveConnections();
    bool setupServerSocket();
    void updateConnectionStats();
};

#endif // QUIC_PROXY_H
