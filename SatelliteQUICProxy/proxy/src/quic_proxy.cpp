/**
 * @file quic_proxy.cpp
 * @brief Implementation of the QUIC proxy class
 */

#include "quic_proxy.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <algorithm>
#include <cstring>

QUICProxy::QUICProxy(const std::string& bindAddress, int port, FPGAInterface& fpgaInterface)
    : m_bindAddress(bindAddress),
      m_port(port),
      m_fpgaInterface(fpgaInterface),
      m_accelerationEnabled(true),
      m_verboseLogging(false),
      m_running(false),
      m_serverSocket(-1),
      m_totalBytesReceived(0),
      m_totalBytesSent(0),
      m_totalPacketsReceived(0),
      m_totalPacketsSent(0),
      m_totalConnections(0),
      m_activeConnections(0) {
}

QUICProxy::~QUICProxy() {
    stop();
}

bool QUICProxy::start() {
    if (m_running.load()) {
        std::cerr << "QUIC proxy already running" << std::endl;
        return false;
    }
    
    // Setup server socket
    if (!setupServerSocket()) {
        return false;
    }
    
    // Start the accept thread
    m_running.store(true);
    m_acceptThread = std::make_unique<std::thread>(&QUICProxy::acceptLoop, this);
    
    if (m_verboseLogging) {
        std::cout << "QUIC proxy started successfully" << std::endl;
        std::cout << "FPGA acceleration: " << (m_accelerationEnabled ? "enabled" : "disabled") << std::endl;
    }
    
    return true;
}

void QUICProxy::stop() {
    if (!m_running.load()) {
        return;
    }
    
    // Signal the accept thread to stop
    m_running.store(false);
    
    // Close server socket to unblock accept()
    if (m_serverSocket != -1) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }
    
    // Wait for accept thread to finish
    if (m_acceptThread && m_acceptThread->joinable()) {
        m_acceptThread->join();
        m_acceptThread.reset();
    }
    
    // Close all active connections
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        m_connections.clear();
    }
    
    if (m_verboseLogging) {
        std::cout << "QUIC proxy stopped" << std::endl;
    }
}

void QUICProxy::setAccelerationEnabled(bool enabled) {
    m_accelerationEnabled = enabled;
    
    if (m_verboseLogging) {
        std::cout << "FPGA acceleration " << (enabled ? "enabled" : "disabled") << std::endl;
    }
    
    // Update all active handlers with the new acceleration setting
    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    for (auto& handler : m_connections) {
        handler->setAccelerationEnabled(enabled);
    }
}

void QUICProxy::setVerboseLogging(bool verbose) {
    m_verboseLogging = verbose;
}

void QUICProxy::printStats() const {
    std::cout << "===== QUIC Proxy Statistics =====" << std::endl;
    std::cout << "Active connections: " << m_activeConnections.load() << std::endl;
    std::cout << "Total connections: " << m_totalConnections.load() << std::endl;
    std::cout << "Packets received: " << m_totalPacketsReceived.load() << std::endl;
    std::cout << "Packets sent: " << m_totalPacketsSent.load() << std::endl;
    std::cout << "Bytes received: " << m_totalBytesReceived.load() << std::endl;
    std::cout << "Bytes sent: " << m_totalBytesSent.load() << std::endl;
    
    // Get FPGA stats if acceleration is enabled
    if (m_accelerationEnabled) {
        m_fpgaInterface.printStats();
    }
    
    std::cout << "=================================" << std::endl;
}

bool QUICProxy::setupServerSocket() {
    // Create UDP socket for QUIC
    m_serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_serverSocket < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    int optval = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    // Bind the socket
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);
    
    if (inet_pton(AF_INET, m_bindAddress.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << m_bindAddress << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    if (bind(m_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Failed to bind socket: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    // Set non-blocking mode
    int flags = fcntl(m_serverSocket, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "Failed to get socket flags: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    if (fcntl(m_serverSocket, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "Failed to set non-blocking mode: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    return true;
}

void QUICProxy::acceptLoop() {
    const int BUFFER_SIZE = 8192;
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    
    while (m_running.load()) {
        // Perform periodic cleanup of inactive connections
        cleanupInactiveConnections();
        
        // Update connection statistics
        updateConnectionStats();
        
        // Wait for incoming packets with select
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(m_serverSocket, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;  // 100ms timeout
        
        int activity = select(m_serverSocket + 1, &readfds, nullptr, nullptr, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            std::cerr << "Select error: " << strerror(errno) << std::endl;
            break;
        }
        
        if (activity == 0 || !FD_ISSET(m_serverSocket, &readfds)) {
            // No data or timeout
            continue;
        }
        
        // Receive packet
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        
        ssize_t bytesRead = recvfrom(
            m_serverSocket,
            buffer.data(),
            buffer.size(),
            0,
            (struct sockaddr*)&clientAddr,
            &clientAddrLen
        );
        
        if (bytesRead < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "Error receiving data: " << strerror(errno) << std::endl;
            }
            continue;
        }
        
        // Update statistics
        m_totalBytesReceived += bytesRead;
        m_totalPacketsReceived++;
        
        // Convert client address to string for lookup
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        std::string clientKey = std::string(clientIP) + ":" + std::to_string(ntohs(clientAddr.sin_port));
        
        // Find or create handler for this client
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        
        // Look for existing connection
        auto it = std::find_if(m_connections.begin(), m_connections.end(),
            [&clientKey](const std::unique_ptr<QUICHandler>& handler) {
                return handler->getClientKey() == clientKey;
            });
        
        QUICHandler* handler = nullptr;
        
        if (it == m_connections.end()) {
            // New connection
            if (m_verboseLogging) {
                std::cout << "New connection from " << clientKey << std::endl;
            }
            
            auto newHandler = std::make_unique<QUICHandler>(
                m_serverSocket,
                clientAddr,
                m_fpgaInterface,
                m_accelerationEnabled
            );
            
            handler = newHandler.get();
            m_connections.push_back(std::move(newHandler));
            m_totalConnections++;
            m_activeConnections++;
        } else {
            // Existing connection
            handler = it->get();
        }
        
        // Process the packet
        if (handler) {
            handler->processIncomingPacket(buffer.data(), bytesRead);
        }
    }
}

void QUICProxy::cleanupInactiveConnections() {
    static auto lastCleanupTime = std::chrono::steady_clock::now();
    auto currentTime = std::chrono::steady_clock::now();
    
    // Only run cleanup every few seconds
    if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastCleanupTime).count() < 5) {
        return;
    }
    
    lastCleanupTime = currentTime;
    
    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    
    // Remove connections that are inactive
    auto it = std::remove_if(m_connections.begin(), m_connections.end(),
        [](const std::unique_ptr<QUICHandler>& handler) {
            return !handler->isActive();
        });
    
    if (it != m_connections.end()) {
        size_t removedCount = std::distance(it, m_connections.end());
        m_connections.erase(it, m_connections.end());
        
        if (m_verboseLogging && removedCount > 0) {
            std::cout << "Cleaned up " << removedCount << " inactive connections" << std::endl;
        }
        
        m_activeConnections = m_connections.size();
    }
}

void QUICProxy::updateConnectionStats() {
    uint64_t bytesSent = 0;
    uint64_t packetsSent = 0;
    
    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    for (const auto& handler : m_connections) {
        bytesSent += handler->getBytesSent();
        packetsSent += handler->getPacketsSent();
    }
    
    m_totalBytesSent.store(bytesSent);
    m_totalPacketsSent.store(packetsSent);
}
