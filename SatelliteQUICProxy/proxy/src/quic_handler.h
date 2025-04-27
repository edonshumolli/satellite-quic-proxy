/**
 * @file quic_handler.h
 * @brief QUIC connection handler class
 * 
 * This class handles individual QUIC connections, managing the protocol state
 * and implementing the QUIC operations with optional FPGA acceleration.
 */

#ifndef QUIC_HANDLER_H
#define QUIC_HANDLER_H

#include <string>
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include "fpga_interface.h"

/**
 * @enum QUICPacketType
 * @brief Types of QUIC packets
 */
enum class QUICPacketType {
    INITIAL,
    HANDSHAKE,
    ZERO_RTT,
    ONE_RTT,
    RETRY,
    VERSION_NEGOTIATION
};

/**
 * @enum QUICFrameType
 * @brief Types of QUIC frames
 */
enum class QUICFrameType {
    PADDING,
    PING,
    ACK,
    RESET_STREAM,
    STOP_SENDING,
    CRYPTO,
    NEW_TOKEN,
    STREAM,
    MAX_DATA,
    MAX_STREAM_DATA,
    MAX_STREAMS,
    DATA_BLOCKED,
    STREAM_DATA_BLOCKED,
    STREAMS_BLOCKED,
    NEW_CONNECTION_ID,
    RETIRE_CONNECTION_ID,
    PATH_CHALLENGE,
    PATH_RESPONSE,
    CONNECTION_CLOSE,
    HANDSHAKE_DONE
};

/**
 * @struct QUICPacketHeader
 * @brief Structure representing a QUIC packet header
 */
struct QUICPacketHeader {
    QUICPacketType type;
    uint32_t version;
    std::vector<uint8_t> destConnID;
    std::vector<uint8_t> srcConnID;
    std::vector<uint8_t> token;
    uint64_t length;
    uint32_t packetNumber;
};

/**
 * @class QUICHandler
 * @brief Handles a single QUIC connection
 */
class QUICHandler {
public:
    /**
     * Constructor
     * 
     * @param socket The socket to use for communication
     * @param clientAddr The client address
     * @param fpgaInterface Reference to the FPGA interface for hardware acceleration
     * @param accelerationEnabled Whether hardware acceleration is enabled
     */
    QUICHandler(int socket, const sockaddr_in& clientAddr, 
               FPGAInterface& fpgaInterface, bool accelerationEnabled);
    
    /**
     * Get a unique key for this client connection
     * 
     * @return A string key in the format "ip:port"
     */
    std::string getClientKey() const;
    
    /**
     * Process an incoming QUIC packet
     * 
     * @param data Pointer to the packet data
     * @param length Length of the packet data
     * @return true if processed successfully, false otherwise
     */
    bool processIncomingPacket(const uint8_t* data, size_t length);
    
    /**
     * Check if the connection is still active
     * 
     * @return true if active, false if idle/closed
     */
    bool isActive() const;
    
    /**
     * Enable or disable FPGA acceleration for this connection
     * 
     * @param enabled Set to true to enable acceleration
     */
    void setAccelerationEnabled(bool enabled);
    
    /**
     * Get the total number of bytes sent by this connection
     * 
     * @return Byte count
     */
    uint64_t getBytesSent() const;
    
    /**
     * Get the total number of packets sent by this connection
     * 
     * @return Packet count
     */
    uint64_t getPacketsSent() const;

private:
    // Socket and addressing
    int m_socket;
    sockaddr_in m_clientAddr;
    std::string m_clientKey;
    
    // FPGA acceleration
    FPGAInterface& m_fpgaInterface;
    bool m_accelerationEnabled;
    
    // Connection state
    bool m_connected;
    std::chrono::steady_clock::time_point m_lastActivity;
    std::atomic<uint64_t> m_bytesSent;
    std::atomic<uint64_t> m_bytesReceived;
    std::atomic<uint64_t> m_packetsSent;
    std::atomic<uint64_t> m_packetsReceived;
    
    // QUIC protocol state
    uint64_t m_connectionID;
    uint32_t m_nextPacketNumber;
    std::vector<uint8_t> m_destConnID;
    std::vector<uint8_t> m_srcConnID;
    
    // Crypto state
    std::vector<uint8_t> m_handshakeSecret;
    std::vector<uint8_t> m_oneRttSecret;
    
    // Packet tracking
    struct PacketInfo {
        uint32_t packetNumber;
        std::chrono::steady_clock::time_point sentTime;
        bool acknowledged;
        std::vector<uint8_t> packetData;
    };
    
    std::vector<PacketInfo> m_sentPackets;
    std::vector<uint32_t> m_receivedPackets;
    std::mutex m_packetsMutex;
    
    // Private helper methods
    bool parsePacket(const uint8_t* data, size_t length, QUICPacketHeader& header, 
                    std::vector<uint8_t>& payload);
    bool handleInitialPacket(const QUICPacketHeader& header, const std::vector<uint8_t>& payload);
    bool handleHandshakePacket(const QUICPacketHeader& header, const std::vector<uint8_t>& payload);
    bool handleOneRttPacket(const QUICPacketHeader& header, const std::vector<uint8_t>& payload);
    bool processFrames(const std::vector<uint8_t>& payload);
    bool sendPacket(QUICPacketType type, const std::vector<uint8_t>& payload);
    bool sendAckPacket(const std::vector<uint32_t>& packetNumbers);
    void updateActivity();
    void checkForRetransmissions();
};

#endif // QUIC_HANDLER_H
