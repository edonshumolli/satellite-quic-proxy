/**
 * @file quic_handler.cpp
 * @brief Implementation of QUIC connection handler
 */

#include "quic_handler.h"
#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <sstream>
#include <algorithm>
#include <chrono>

// Constants for QUIC protocol
const uint32_t QUIC_VERSION_1 = 0x00000001;  // Version 1
const size_t MAX_CONN_ID_LEN = 20;           // Maximum Connection ID length
const size_t MIN_INITIAL_SIZE = 1200;        // Minimum size for Initial packets
const uint32_t IDLE_TIMEOUT_MS = 30000;      // 30 seconds idle timeout
const uint32_t RETRANSMIT_TIMEOUT_MS = 500;  // Initial 500ms retransmission timeout

QUICHandler::QUICHandler(int socket, const sockaddr_in& clientAddr, 
                       FPGAInterface& fpgaInterface, bool accelerationEnabled)
    : m_socket(socket),
      m_clientAddr(clientAddr),
      m_fpgaInterface(fpgaInterface),
      m_accelerationEnabled(accelerationEnabled),
      m_connected(true),
      m_bytesSent(0),
      m_bytesReceived(0),
      m_packetsSent(0),
      m_packetsReceived(0),
      m_nextPacketNumber(0) {
    
    // Create a client key from the address
    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
    m_clientKey = std::string(clientIP) + ":" + std::to_string(ntohs(clientAddr.sin_port));
    
    // Generate random connection ID (8 bytes)
    std::vector<uint8_t> connID(8);
    RAND_bytes(connID.data(), connID.size());
    m_connectionID = 0;
    for (size_t i = 0; i < connID.size(); i++) {
        m_connectionID = (m_connectionID << 8) | connID[i];
    }
    
    // Initialize connection IDs
    m_srcConnID = connID;
    
    // Initialize timestamp
    updateActivity();
}

std::string QUICHandler::getClientKey() const {
    return m_clientKey;
}

bool QUICHandler::processIncomingPacket(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return false;
    }
    
    // Update activity timestamp
    updateActivity();
    
    // Update received bytes
    m_bytesReceived += length;
    m_packetsReceived++;
    
    // Parse the packet
    QUICPacketHeader header;
    std::vector<uint8_t> payload;
    
    if (!parsePacket(data, length, header, payload)) {
        std::cerr << "Failed to parse QUIC packet from " << m_clientKey << std::endl;
        return false;
    }
    
    // Store destination connection ID if first packet
    if (m_destConnID.empty()) {
        m_destConnID = header.destConnID;
    }
    
    // Handle different packet types
    bool result = false;
    switch (header.type) {
        case QUICPacketType::INITIAL:
            result = handleInitialPacket(header, payload);
            break;
            
        case QUICPacketType::HANDSHAKE:
            result = handleHandshakePacket(header, payload);
            break;
            
        case QUICPacketType::ONE_RTT:
            result = handleOneRttPacket(header, payload);
            break;
            
        case QUICPacketType::ZERO_RTT:
            // Not implemented in this simplified version
            std::cerr << "0-RTT packets not supported yet" << std::endl;
            break;
            
        case QUICPacketType::RETRY:
            // Not implemented in this simplified version
            std::cerr << "Retry packets not supported yet" << std::endl;
            break;
            
        case QUICPacketType::VERSION_NEGOTIATION:
            // Not implemented in this simplified version
            std::cerr << "Version negotiation packets not supported yet" << std::endl;
            break;
            
        default:
            std::cerr << "Unknown packet type" << std::endl;
            break;
    }
    
    // Check for packets that need retransmission
    checkForRetransmissions();
    
    return result;
}

bool QUICHandler::isActive() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastActivity).count();
    
    return m_connected && (elapsed < IDLE_TIMEOUT_MS);
}

void QUICHandler::setAccelerationEnabled(bool enabled) {
    m_accelerationEnabled = enabled;
}

uint64_t QUICHandler::getBytesSent() const {
    return m_bytesSent.load();
}

uint64_t QUICHandler::getPacketsSent() const {
    return m_packetsSent.load();
}

bool QUICHandler::parsePacket(const uint8_t* data, size_t length, 
                             QUICPacketHeader& header, std::vector<uint8_t>& payload) {
    if (length < 4) {
        std::cerr << "Packet too short" << std::endl;
        return false;
    }
    
    // First byte contains the header form and packet type
    uint8_t firstByte = data[0];
    bool longHeader = (firstByte & 0x80) != 0;  // MSB indicates long/short header
    
    size_t offset = 1;  // Start after the first byte
    
    if (longHeader) {
        // Long header format
        uint8_t packetType = (firstByte & 0x30) >> 4;  // Bits 5-4 indicate type
        
        // Map to QUIC packet type
        switch (packetType) {
            case 0: header.type = QUICPacketType::INITIAL; break;
            case 1: header.type = QUICPacketType::ZERO_RTT; break;
            case 2: header.type = QUICPacketType::HANDSHAKE; break;
            case 3: header.type = QUICPacketType::RETRY; break;
            default: return false;  // Invalid type
        }
        
        // Version (4 bytes)
        if (offset + 4 > length) return false;
        header.version = (data[offset] << 24) | (data[offset + 1] << 16) | 
                         (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        
        // If version is 0, this is a Version Negotiation packet
        if (header.version == 0) {
            header.type = QUICPacketType::VERSION_NEGOTIATION;
        }
        
        // Destination Connection ID Length (1 byte)
        if (offset + 1 > length) return false;
        uint8_t destConnIDLen = data[offset++];
        
        // Destination Connection ID
        if (offset + destConnIDLen > length) return false;
        header.destConnID.assign(data + offset, data + offset + destConnIDLen);
        offset += destConnIDLen;
        
        // Source Connection ID Length (1 byte)
        if (offset + 1 > length) return false;
        uint8_t srcConnIDLen = data[offset++];
        
        // Source Connection ID
        if (offset + srcConnIDLen > length) return false;
        header.srcConnID.assign(data + offset, data + offset + srcConnIDLen);
        offset += srcConnIDLen;
        
        if (header.type == QUICPacketType::INITIAL) {
            // Token Length (variable-length integer)
            if (offset + 1 > length) return false;
            uint64_t tokenLen = data[offset++];
            
            // Check for multi-byte length encoding
            if (tokenLen > 0x3F) {
                // This is a simplified implementation
                // In a full implementation, we would properly decode variable-length integers
                std::cerr << "Variable-length token length not fully implemented" << std::endl;
                return false;
            }
            
            // Token
            if (offset + tokenLen > length) return false;
            header.token.assign(data + offset, data + offset + tokenLen);
            offset += tokenLen;
        }
        
        // Length (variable-length integer)
        if (offset + 1 > length) return false;
        uint64_t payloadLen = data[offset++];
        
        // Check for multi-byte length encoding
        if (payloadLen > 0x3F) {
            // This is a simplified implementation
            // In a full implementation, we would properly decode variable-length integers
            std::cerr << "Variable-length payload length not fully implemented" << std::endl;
            return false;
        }
        
        header.length = payloadLen;
        
        // Packet Number (1-4 bytes, assume 4 for simplicity)
        if (offset + 4 > length) return false;
        header.packetNumber = (data[offset] << 24) | (data[offset + 1] << 16) | 
                             (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        
    } else {
        // Short header format (1-RTT)
        header.type = QUICPacketType::ONE_RTT;
        
        // Destination Connection ID (fixed length for this implementation)
        // In a real implementation, the length would be known from the connection state
        const size_t DCID_LEN = 8;  // Assume 8-byte connection IDs
        
        if (offset + DCID_LEN > length) return false;
        header.destConnID.assign(data + offset, data + offset + DCID_LEN);
        offset += DCID_LEN;
        
        // Packet Number (1-4 bytes, assume 4 for simplicity)
        if (offset + 4 > length) return false;
        header.packetNumber = (data[offset] << 24) | (data[offset + 1] << 16) | 
                             (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        
        // No explicit length field in short header, it's the rest of the datagram
        header.length = length - offset;
    }
    
    // Extract payload
    if (offset < length) {
        payload.assign(data + offset, data + length);
    } else {
        payload.clear();
    }
    
    return true;
}

bool QUICHandler::handleInitialPacket(const QUICPacketHeader& header, const std::vector<uint8_t>& payload) {
    // Track received packet number
    {
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        m_receivedPackets.push_back(header.packetNumber);
    }
    
    // In a real implementation, we would:
    // 1. Decrypt the payload using Initial secrets
    // 2. Process CRYPTO frames to continue the TLS handshake
    // 3. Generate and send a response
    
    // For this simplified implementation, we'll just respond with our own Initial packet
    
    // Create a simple CRYPTO frame as payload (in a real implementation this would be TLS data)
    std::vector<uint8_t> responsePayload = {
        0x06,  // CRYPTO frame type
        0x00,  // Offset (0)
        0x10,  // Length (16 bytes)
        // 16 bytes of sample crypto data (would be real TLS in actual implementation)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
    };
    
    // If FPGA acceleration is enabled, use it for packet framing
    if (m_accelerationEnabled && m_fpgaInterface.isConnected()) {
        FPGAOperationResult result = m_fpgaInterface.executePacketOperation(
            FPGAOperationType::PACKET_FRAMING,
            responsePayload,
            m_connectionID,
            m_nextPacketNumber
        );
        
        if (result.success) {
            // Send the FPGA-framed packet
            sendto(m_socket, result.data.data(), result.data.size(), 0,
                  (struct sockaddr*)&m_clientAddr, sizeof(m_clientAddr));
            
            // Update statistics
            m_bytesSent += result.data.size();
            m_packetsSent++;
            
            // Store packet for potential retransmission
            {
                std::lock_guard<std::mutex> lock(m_packetsMutex);
                PacketInfo packetInfo;
                packetInfo.packetNumber = m_nextPacketNumber;
                packetInfo.sentTime = std::chrono::steady_clock::now();
                packetInfo.acknowledged = false;
                packetInfo.packetData = result.data;
                m_sentPackets.push_back(packetInfo);
            }
            
            m_nextPacketNumber++;
            return true;
        }
    }
    
    // If acceleration failed or is disabled, fall back to software implementation
    return sendPacket(QUICPacketType::INITIAL, responsePayload);
}

bool QUICHandler::handleHandshakePacket(const QUICPacketHeader& header, const std::vector<uint8_t>& payload) {
    // Track received packet number
    {
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        m_receivedPackets.push_back(header.packetNumber);
    }
    
    // In a real implementation, we would:
    // 1. Decrypt the payload using Handshake secrets
    // 2. Process CRYPTO frames to continue the TLS handshake
    // 3. Generate and send a response
    
    // For this simplified implementation, we'll respond with a Handshake packet and then a 1-RTT packet
    
    // Create a simple CRYPTO frame as payload (in a real implementation this would be TLS data)
    std::vector<uint8_t> responsePayload = {
        0x06,  // CRYPTO frame type
        0x00,  // Offset (0)
        0x10,  // Length (16 bytes)
        // 16 bytes of sample crypto data (would be real TLS in actual implementation)
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20
    };
    
    // Send handshake packet
    bool handshakeResult = sendPacket(QUICPacketType::HANDSHAKE, responsePayload);
    
    // Send a 1-RTT packet to indicate handshake completion
    std::vector<uint8_t> oneRttPayload = {
        0x1E  // HANDSHAKE_DONE frame type
    };
    
    bool oneRttResult = sendPacket(QUICPacketType::ONE_RTT, oneRttPayload);
    
    return handshakeResult && oneRttResult;
}

bool QUICHandler::handleOneRttPacket(const QUICPacketHeader& header, const std::vector<uint8_t>& payload) {
    // Track received packet number
    {
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        m_receivedPackets.push_back(header.packetNumber);
    }
    
    // Process frames in the payload
    if (!processFrames(payload)) {
        return false;
    }
    
    // Acknowledge the packet
    std::vector<uint32_t> packetsToAck;
    {
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        packetsToAck = m_receivedPackets;
    }
    
    return sendAckPacket(packetsToAck);
}

bool QUICHandler::processFrames(const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        return true;  // Nothing to process
    }
    
    // In a real implementation, we would iterate through the frames
    // For this simplified implementation, we'll just check for some common frame types
    
    size_t offset = 0;
    while (offset < payload.size()) {
        // Get frame type
        uint8_t frameType = payload[offset++];
        
        switch (frameType) {
            case 0x00:  // PADDING
                // No content, just skip
                break;
                
            case 0x01:  // PING
                // No content, just respond with ACK
                break;
                
            case 0x02:  // ACK
            case 0x03:  // ACK with ECN
                // Process ACK frame
                // In a real implementation, we would parse ACK ranges and update our sent packets
                // For simplicity, we'll just mark the first mentioned packet as acknowledged
                if (offset + 4 <= payload.size()) {
                    uint32_t ackedPacketNum = (payload[offset] << 24) | (payload[offset + 1] << 16) |
                                              (payload[offset + 2] << 8) | payload[offset + 3];
                    
                    std::lock_guard<std::mutex> lock(m_packetsMutex);
                    for (auto& packetInfo : m_sentPackets) {
                        if (packetInfo.packetNumber == ackedPacketNum) {
                            packetInfo.acknowledged = true;
                            break;
                        }
                    }
                }
                
                // Skip the rest of the ACK frame (simplified)
                offset = payload.size();
                break;
                
            case 0x06:  // CRYPTO
                // Process CRYPTO frame (would be part of TLS handshake)
                // In a real implementation, we would extract offset, length, and data
                // Skip for this simplified implementation
                offset = payload.size();
                break;
                
            case 0x08:  // STREAM (base value, real frames may have bits set)
            case 0x09:
            case 0x0A:
            case 0x0B:
            case 0x0C:
            case 0x0D:
            case 0x0E:
            case 0x0F:
                // Process STREAM frame
                // In a real implementation, we would extract stream ID, offset, and data
                // For simplicity, we'll just echo back the data
                
                // Skip to the end for this simplified implementation
                offset = payload.size();
                
                // Echo the data back in a new STREAM frame
                std::vector<uint8_t> responsePayload = payload;
                return sendPacket(QUICPacketType::ONE_RTT, responsePayload);
                
            default:
                // Skip unhandled frame types
                offset = payload.size();
                break;
        }
    }
    
    return true;
}

bool QUICHandler::sendPacket(QUICPacketType type, const std::vector<uint8_t>& payload) {
    // If FPGA acceleration is enabled and available, use it for packet framing
    if (m_accelerationEnabled && m_fpgaInterface.isConnected()) {
        FPGAOperationResult result = m_fpgaInterface.executePacketOperation(
            FPGAOperationType::PACKET_FRAMING,
            payload,
            m_connectionID,
            m_nextPacketNumber
        );
        
        if (result.success) {
            // Send the FPGA-framed packet
            sendto(m_socket, result.data.data(), result.data.size(), 0,
                  (struct sockaddr*)&m_clientAddr, sizeof(m_clientAddr));
            
            // Update statistics
            m_bytesSent += result.data.size();
            m_packetsSent++;
            
            // Store packet for potential retransmission
            {
                std::lock_guard<std::mutex> lock(m_packetsMutex);
                PacketInfo packetInfo;
                packetInfo.packetNumber = m_nextPacketNumber;
                packetInfo.sentTime = std::chrono::steady_clock::now();
                packetInfo.acknowledged = false;
                packetInfo.packetData = result.data;
                m_sentPackets.push_back(packetInfo);
            }
            
            m_nextPacketNumber++;
            return true;
        }
    }
    
    // Fall back to software implementation if FPGA framing failed or is disabled
    
    // Calculate approximate packet size to allocate buffer
    size_t packetSize = payload.size() + 64;  // Add space for headers
    std::vector<uint8_t> packetData(packetSize);
    
    size_t offset = 0;
    
    // Set initial byte based on packet type
    switch (type) {
        case QUICPacketType::INITIAL:
            packetData[offset++] = 0xC3;  // Long header + Initial type
            break;
            
        case QUICPacketType::HANDSHAKE:
            packetData[offset++] = 0xE3;  // Long header + Handshake type
            break;
            
        case QUICPacketType::ZERO_RTT:
            packetData[offset++] = 0xD3;  // Long header + 0-RTT type
            break;
            
        case QUICPacketType::ONE_RTT:
            packetData[offset++] = 0x40;  // Short header format
            break;
            
        default:
            std::cerr << "Unsupported packet type for sending" << std::endl;
            return false;
    }
    
    if (type != QUICPacketType::ONE_RTT) {
        // Long header format
        
        // Version (4 bytes)
        packetData[offset++] = (QUIC_VERSION_1 >> 24) & 0xFF;
        packetData[offset++] = (QUIC_VERSION_1 >> 16) & 0xFF;
        packetData[offset++] = (QUIC_VERSION_1 >> 8) & 0xFF;
        packetData[offset++] = QUIC_VERSION_1 & 0xFF;
        
        // DCID Length
        packetData[offset++] = m_destConnID.size();
        
        // DCID
        std::copy(m_destConnID.begin(), m_destConnID.end(), packetData.begin() + offset);
        offset += m_destConnID.size();
        
        // SCID Length
        packetData[offset++] = m_srcConnID.size();
        
        // SCID
        std::copy(m_srcConnID.begin(), m_srcConnID.end(), packetData.begin() + offset);
        offset += m_srcConnID.size();
        
        if (type == QUICPacketType::INITIAL) {
            // Token Length (no token for responses)
            packetData[offset++] = 0;
        }
        
        // Payload Length (simplified - in a real implementation, this would be a variable-length integer)
        // Reserve 2 bytes for the length, will fill in actual value later
        size_t lengthOffset = offset;
        packetData[offset++] = 0;
        packetData[offset++] = 0;
        
        // Packet Number (4 bytes)
        packetData[offset++] = (m_nextPacketNumber >> 24) & 0xFF;
        packetData[offset++] = (m_nextPacketNumber >> 16) & 0xFF;
        packetData[offset++] = (m_nextPacketNumber >> 8) & 0xFF;
        packetData[offset++] = m_nextPacketNumber & 0xFF;
        
        // Payload
        std::copy(payload.begin(), payload.end(), packetData.begin() + offset);
        size_t finalOffset = offset + payload.size();
        
        // Go back and fill in the payload length
        size_t payloadLength = finalOffset - lengthOffset - 2;
        packetData[lengthOffset] = (payloadLength >> 8) & 0xFF;
        packetData[lengthOffset + 1] = payloadLength & 0xFF;
        
        // Resize packet to actual size
        packetData.resize(finalOffset);
        
    } else {
        // Short header format (1-RTT)
        
        // DCID
        std::copy(m_destConnID.begin(), m_destConnID.end(), packetData.begin() + offset);
        offset += m_destConnID.size();
        
        // Packet Number (4 bytes)
        packetData[offset++] = (m_nextPacketNumber >> 24) & 0xFF;
        packetData[offset++] = (m_nextPacketNumber >> 16) & 0xFF;
        packetData[offset++] = (m_nextPacketNumber >> 8) & 0xFF;
        packetData[offset++] = m_nextPacketNumber & 0xFF;
        
        // Payload
        std::copy(payload.begin(), payload.end(), packetData.begin() + offset);
        offset += payload.size();
        
        // Resize packet to actual size
        packetData.resize(offset);
    }
    
    // In a real implementation, we would encrypt the packet here
    
    // Send the packet
    sendto(m_socket, packetData.data(), packetData.size(), 0,
          (struct sockaddr*)&m_clientAddr, sizeof(m_clientAddr));
    
    // Update statistics
    m_bytesSent += packetData.size();
    m_packetsSent++;
    
    // Store packet for potential retransmission
    {
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        PacketInfo packetInfo;
        packetInfo.packetNumber = m_nextPacketNumber;
        packetInfo.sentTime = std::chrono::steady_clock::now();
        packetInfo.acknowledged = false;
        packetInfo.packetData = packetData;
        m_sentPackets.push_back(packetInfo);
    }
    
    m_nextPacketNumber++;
    return true;
}

bool QUICHandler::sendAckPacket(const std::vector<uint32_t>& packetNumbers) {
    if (packetNumbers.empty()) {
        return true;  // Nothing to acknowledge
    }
    
    // Find highest packet number
    uint32_t largestAcked = *std::max_element(packetNumbers.begin(), packetNumbers.end());
    
    // Create ACK frame
    // Note: This is a simplified ACK frame format
    std::vector<uint8_t> ackFrame = {
        0x02,  // ACK frame type
        
        // Largest Acknowledged (4 bytes)
        (uint8_t)((largestAcked >> 24) & 0xFF),
        (uint8_t)((largestAcked >> 16) & 0xFF),
        (uint8_t)((largestAcked >> 8) & 0xFF),
        (uint8_t)(largestAcked & 0xFF),
        
        // ACK Delay (2 bytes, set to 0 for simplicity)
        0x00, 0x00,
        
        // ACK Range Count (1 byte, set to 0 for simplicity)
        0x00,
        
        // First ACK Range (1 byte, set to 0 for simplicity)
        0x00
    };
    
    // If FPGA acceleration is enabled, use it for ACK processing
    if (m_accelerationEnabled && m_fpgaInterface.isConnected()) {
        FPGAOperationResult result = m_fpgaInterface.executePacketOperation(
            FPGAOperationType::PACKET_ACK_PROCESSING,
            ackFrame,
            m_connectionID,
            largestAcked
        );
        
        if (result.success) {
            // Send the FPGA-generated ACK packet
            sendto(m_socket, result.data.data(), result.data.size(), 0,
                  (struct sockaddr*)&m_clientAddr, sizeof(m_clientAddr));
            
            // Update statistics
            m_bytesSent += result.data.size();
            m_packetsSent++;
            
            return true;
        }
    }
    
    // Fall back to software implementation if FPGA processing failed or is disabled
    return sendPacket(QUICPacketType::ONE_RTT, ackFrame);
}

void QUICHandler::updateActivity() {
    m_lastActivity = std::chrono::steady_clock::now();
}

void QUICHandler::checkForRetransmissions() {
    auto now = std::chrono::steady_clock::now();
    std::vector<PacketInfo> packetsToRetransmit;
    
    // Check for packets that need retransmission
    {
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        for (const auto& packetInfo : m_sentPackets) {
            if (!packetInfo.acknowledged) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - packetInfo.sentTime).count();
                
                if (elapsed > RETRANSMIT_TIMEOUT_MS) {
                    packetsToRetransmit.push_back(packetInfo);
                }
            }
        }
    }
    
    // Retransmit packets if needed
    for (const auto& packetInfo : packetsToRetransmit) {
        // If FPGA acceleration is enabled, use it for retransmission
        if (m_accelerationEnabled && m_fpgaInterface.isConnected()) {
            FPGAOperationResult result = m_fpgaInterface.executePacketOperation(
                FPGAOperationType::PACKET_RETRANSMISSION,
                {},  // Empty input, the FPGA will use the packet number to find the packet
                m_connectionID,
                packetInfo.packetNumber
            );
            
            if (result.success && !result.data.empty()) {
                // Send the FPGA-regenerated packet
                sendto(m_socket, result.data.data(), result.data.size(), 0,
                      (struct sockaddr*)&m_clientAddr, sizeof(m_clientAddr));
                
                // Update statistics
                m_bytesSent += result.data.size();
                m_packetsSent++;
                
                // Update sent packet info
                std::lock_guard<std::mutex> lock(m_packetsMutex);
                for (auto& info : m_sentPackets) {
                    if (info.packetNumber == packetInfo.packetNumber) {
                        info.sentTime = now;
                        break;
                    }
                }
                
                continue;  // Skip software retransmission
            }
        }
        
        // Fall back to software retransmission if FPGA processing failed or is disabled
        // Simply resend the original packet
        sendto(m_socket, packetInfo.packetData.data(), packetInfo.packetData.size(), 0,
              (struct sockaddr*)&m_clientAddr, sizeof(m_clientAddr));
        
        // Update statistics
        m_bytesSent += packetInfo.packetData.size();
        m_packetsSent++;
        
        // Update sent packet info
        std::lock_guard<std::mutex> lock(m_packetsMutex);
        for (auto& info : m_sentPackets) {
            if (info.packetNumber == packetInfo.packetNumber) {
                info.sentTime = now;
                break;
            }
        }
    }
}
