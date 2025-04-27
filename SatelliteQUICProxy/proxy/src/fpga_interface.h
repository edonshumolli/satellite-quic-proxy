/**
 * @file fpga_interface.h
 * @brief Interface for communicating with the FPGA accelerator
 * 
 * This class provides a high-level interface for the QUIC proxy to interact
 * with the FPGA-based hardware accelerator for QUIC protocol operations.
 */

#ifndef FPGA_INTERFACE_H
#define FPGA_INTERFACE_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>

// Forward declaration
namespace DMA {
    class Controller;
}

/**
 * @enum FPGAOperationType
 * @brief Types of QUIC operations that can be offloaded to FPGA
 */
enum class FPGAOperationType {
    CRYPTO_ENCRYPT,
    CRYPTO_DECRYPT,
    COMPRESSION,
    DECOMPRESSION,
    PACKET_FRAMING,
    PACKET_ACK_PROCESSING,
    PACKET_RETRANSMISSION
};

/**
 * @struct FPGAOperationResult
 * @brief Result of an FPGA-accelerated operation
 */
struct FPGAOperationResult {
    bool success;                   // Operation completed successfully
    std::vector<uint8_t> data;      // Output data if applicable
    uint32_t bytesProcessed;        // Number of bytes processed
    double processingTimeMs;        // Processing time in milliseconds
    uint32_t errorCode;             // Error code if operation failed
    std::string errorMessage;       // Error message if operation failed
    
    FPGAOperationResult() : success(false), bytesProcessed(0), processingTimeMs(0), errorCode(0) {}
};

/**
 * @class FPGAInterface
 * @brief Main interface class for interacting with the FPGA hardware
 */
class FPGAInterface {
public:
    /**
     * Constructor for FPGA interface
     * 
     * @param devicePath Path to the FPGA device file
     * @param simulationMode If true, operates in simulation mode without real FPGA
     */
    FPGAInterface(const std::string& devicePath, bool simulationMode = false);
    
    /**
     * Destructor ensures proper cleanup of resources
     */
    ~FPGAInterface();
    
    /**
     * Initialize the FPGA interface
     * 
     * @return true if initialization successful, false otherwise
     */
    bool initialize();
    
    /**
     * Shut down the FPGA interface
     */
    void shutdown();
    
    /**
     * Check if the interface is connected to a working FPGA
     * 
     * @return true if connected and operational
     */
    bool isConnected() const;
    
    /**
     * Execute a crypto operation (encryption/decryption) on the FPGA
     * 
     * @param operationType Type of crypto operation (encrypt/decrypt)
     * @param inputData Input data to process
     * @param key Encryption/decryption key
     * @param nonce Nonce for the operation
     * @param aad Additional authenticated data for AEAD
     * @param callback Callback function for asynchronous completion
     * @return Operation result for synchronous calls, or preliminary status for async
     */
    FPGAOperationResult executeCryptoOperation(
        FPGAOperationType operationType,
        const std::vector<uint8_t>& inputData,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce,
        const std::vector<uint8_t>& aad,
        std::function<void(const FPGAOperationResult&)> callback = nullptr
    );
    
    /**
     * Execute a compression operation on the FPGA
     * 
     * @param operationType Compression or decompression
     * @param inputData Input data to process
     * @param callback Callback function for asynchronous completion
     * @return Operation result for synchronous calls, or preliminary status for async
     */
    FPGAOperationResult executeCompressionOperation(
        FPGAOperationType operationType,
        const std::vector<uint8_t>& inputData,
        std::function<void(const FPGAOperationResult&)> callback = nullptr
    );
    
    /**
     * Execute a packet processing operation on the FPGA
     * 
     * @param operationType Type of packet operation (framing, ACK, retransmission)
     * @param inputData Input data to process
     * @param connectionID QUIC connection ID
     * @param packetNumber QUIC packet number
     * @param callback Callback function for asynchronous completion
     * @return Operation result for synchronous calls, or preliminary status for async
     */
    FPGAOperationResult executePacketOperation(
        FPGAOperationType operationType,
        const std::vector<uint8_t>& inputData,
        uint64_t connectionID,
        uint32_t packetNumber,
        std::function<void(const FPGAOperationResult&)> callback = nullptr
    );
    
    /**
     * Reset the FPGA to a clean state
     * 
     * @return true if reset successful
     */
    bool resetFPGA();
    
    /**
     * Print FPGA statistics
     */
    void printStats() const;

private:
    // FPGA device information
    std::string m_devicePath;
    bool m_simulationMode;
    int m_deviceFd;
    std::atomic<bool> m_connected;
    
    // DMA controller for high-speed data transfer
    std::unique_ptr<DMA::Controller> m_dmaController;
    
    // Memory-mapped registers access mutex
    mutable std::mutex m_regMutex;
    
    // Operation statistics
    mutable std::mutex m_statsMutex;
    std::atomic<uint64_t> m_cryptoOpsCount;
    std::atomic<uint64_t> m_compressionOpsCount;
    std::atomic<uint64_t> m_packetOpsCount;
    std::atomic<uint64_t> m_totalBytesProcessed;
    std::atomic<double> m_totalProcessingTimeMs;
    
    // Private helper methods
    bool openDevice();
    void closeDevice();
    bool writeRegister(uint32_t regAddr, uint32_t value);
    bool readRegister(uint32_t regAddr, uint32_t& value) const;
    void updateStats(uint32_t bytesProcessed, double processingTimeMs);
    
    // Simulation mode implementations
    FPGAOperationResult simulateCryptoOperation(
        FPGAOperationType operationType,
        const std::vector<uint8_t>& inputData,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce,
        const std::vector<uint8_t>& aad
    );
    
    FPGAOperationResult simulateCompressionOperation(
        FPGAOperationType operationType,
        const std::vector<uint8_t>& inputData
    );
    
    FPGAOperationResult simulatePacketOperation(
        FPGAOperationType operationType,
        const std::vector<uint8_t>& inputData,
        uint64_t connectionID,
        uint32_t packetNumber
    );
};

#endif // FPGA_INTERFACE_H
