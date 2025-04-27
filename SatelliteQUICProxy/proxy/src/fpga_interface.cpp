/**
 * @file fpga_interface.cpp
 * @brief Implementation of the FPGA interface for hardware acceleration
 */

#include "fpga_interface.h"
#include "../interface/src/dma_controller.h"
#include <iostream>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <cstring>
#include <thread>
#include <random>
#include <iomanip>
#include <openssl/aes.h>
#include <openssl/evp.h>

// FPGA memory map registers
#define REG_CONTROL             0x0000  // Control register
#define REG_STATUS              0x0004  // Status register
#define REG_MODULE_SELECT       0x0008  // Module selection
#define REG_OP_TYPE             0x000C  // Operation type
#define REG_DATA_ADDR           0x0010  // Data buffer address
#define REG_DATA_SIZE           0x0014  // Data buffer size
#define REG_KEY_ADDR            0x0018  // Key buffer address
#define REG_KEY_SIZE            0x001C  // Key buffer size
#define REG_NONCE_ADDR          0x0020  // Nonce buffer address
#define REG_AAD_ADDR            0x0024  // AAD buffer address
#define REG_AAD_SIZE            0x0028  // AAD buffer size
#define REG_RESULT_ADDR         0x002C  // Result buffer address
#define REG_RESULT_SIZE         0x0030  // Result buffer size
#define REG_CONNECTION_ID_HIGH  0x0034  // Connection ID high 32 bits
#define REG_CONNECTION_ID_LOW   0x0038  // Connection ID low 32 bits
#define REG_PACKET_NUMBER       0x003C  // Packet number
#define REG_PROCESSING_TIME     0x0040  // Processing time in microseconds
#define REG_ERROR_CODE          0x0044  // Error code

// Control register bit definitions
#define CTRL_START              0x00000001  // Start processing
#define CTRL_RESET              0x00000002  // Reset the module
#define CTRL_IRQ_ENABLE         0x00000004  // Enable interrupts
#define CTRL_MODE_MASK          0x00000F00  // Mode bits mask
#define CTRL_MODE_SHIFT         8           // Mode bits shift

// Status register bit definitions
#define STATUS_BUSY             0x00000001  // Busy processing
#define STATUS_DONE             0x00000002  // Processing complete
#define STATUS_ERROR            0x00000004  // Error occurred
#define STATUS_OVERFLOW         0x00000008  // Buffer overflow
#define STATUS_UNDERFLOW        0x00000010  // Buffer underflow
#define STATUS_IRQ              0x00000020  // Interrupt pending

// Module select values
#define MODULE_CRYPTO           0x00000001  // Crypto module
#define MODULE_COMPRESSION      0x00000002  // Compression module
#define MODULE_PACKET           0x00000004  // Packet processor module

// Operation type values
#define OP_CRYPTO_ENCRYPT       0x00000001  // Encrypt operation
#define OP_CRYPTO_DECRYPT       0x00000002  // Decrypt operation
#define OP_COMPRESSION          0x00000001  // Compression operation
#define OP_DECOMPRESSION        0x00000002  // Decompression operation
#define OP_PACKET_FRAME         0x00000001  // Packet framing operation
#define OP_PACKET_ACK           0x00000002  // ACK processing operation
#define OP_PACKET_RETRANSMIT    0x00000003  // Retransmission operation

// Sizes and limits
#define MAX_DMA_BUFFER_SIZE     (4 * 1024 * 1024)  // 4MB max DMA transfer
#define MIN_DMA_BUFFER_SIZE     64                  // 64 bytes min DMA transfer
#define MAX_KEY_SIZE            32                  // 256-bit key max
#define MAX_NONCE_SIZE          12                  // 96-bit nonce max
#define MAX_AAD_SIZE            64                  // 64 bytes AAD max

// Timeouts
#define FPGA_OPERATION_TIMEOUT_MS  5000   // 5 seconds timeout for FPGA operations

FPGAInterface::FPGAInterface(const std::string& devicePath, bool simulationMode)
    : m_devicePath(devicePath),
      m_simulationMode(simulationMode),
      m_deviceFd(-1),
      m_connected(false),
      m_cryptoOpsCount(0),
      m_compressionOpsCount(0),
      m_packetOpsCount(0),
      m_totalBytesProcessed(0),
      m_totalProcessingTimeMs(0) {
}

FPGAInterface::~FPGAInterface() {
    shutdown();
}

bool FPGAInterface::initialize() {
    if (m_simulationMode) {
        std::cout << "Initializing FPGA interface in simulation mode" << std::endl;
        m_connected.store(true);
        return true;
    }
    
    // Open FPGA device
    if (!openDevice()) {
        return false;
    }
    
    // Initialize DMA controller
    try {
        m_dmaController = std::make_unique<DMA::Controller>(m_deviceFd);
        if (!m_dmaController->initialize()) {
            std::cerr << "Failed to initialize DMA controller" << std::endl;
            closeDevice();
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception during DMA controller initialization: " << e.what() << std::endl;
        closeDevice();
        return false;
    }
    
    // Reset FPGA to known state
    if (!resetFPGA()) {
        std::cerr << "Failed to reset FPGA" << std::endl;
        closeDevice();
        return false;
    }
    
    m_connected.store(true);
    std::cout << "FPGA interface initialized successfully" << std::endl;
    return true;
}

void FPGAInterface::shutdown() {
    if (m_simulationMode) {
        m_connected.store(false);
        return;
    }
    
    // Clean up DMA controller
    if (m_dmaController) {
        m_dmaController.reset();
    }
    
    // Close device
    closeDevice();
    m_connected.store(false);
}

bool FPGAInterface::isConnected() const {
    return m_connected.load();
}

FPGAOperationResult FPGAInterface::executeCryptoOperation(
    FPGAOperationType operationType,
    const std::vector<uint8_t>& inputData,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& nonce,
    const std::vector<uint8_t>& aad,
    std::function<void(const FPGAOperationResult&)> callback) {
    
    // Validate parameters
    if (inputData.empty() || key.empty() || nonce.empty()) {
        FPGAOperationResult result;
        result.success = false;
        result.errorCode = 1;
        result.errorMessage = "Invalid parameters: empty input data, key, or nonce";
        return result;
    }
    
    if (key.size() > MAX_KEY_SIZE || nonce.size() > MAX_NONCE_SIZE || 
        (aad.size() > MAX_AAD_SIZE && !aad.empty())) {
        FPGAOperationResult result;
        result.success = false;
        result.errorCode = 2;
        result.errorMessage = "Invalid parameters: key, nonce, or AAD too large";
        return result;
    }
    
    // Record start time for performance measurement
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Use simulation if in simulation mode or not connected
    if (m_simulationMode || !m_connected.load()) {
        auto result = simulateCryptoOperation(operationType, inputData, key, nonce, aad);
        
        // Record end time and calculate duration
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        result.processingTimeMs = duration.count();
        
        // Update statistics
        updateStats(result.bytesProcessed, result.processingTimeMs);
        
        // Call callback if provided
        if (callback) {
            callback(result);
        }
        
        return result;
    }
    
    // Real FPGA implementation
    FPGAOperationResult result;
    result.success = true;
    
    try {
        // Allocate DMA buffers for data transfer
        auto inputBuffer = m_dmaController->allocateBuffer(inputData.size());
        auto keyBuffer = m_dmaController->allocateBuffer(key.size());
        auto nonceBuffer = m_dmaController->allocateBuffer(nonce.size());
        auto aadBuffer = aad.empty() ? nullptr : m_dmaController->allocateBuffer(aad.size());
        auto outputBuffer = m_dmaController->allocateBuffer(inputData.size() + 16);  // +16 for auth tag
        
        // Copy data to DMA buffers
        std::memcpy(inputBuffer->getVirtualAddress(), inputData.data(), inputData.size());
        std::memcpy(keyBuffer->getVirtualAddress(), key.data(), key.size());
        std::memcpy(nonceBuffer->getVirtualAddress(), nonce.data(), nonce.size());
        
        if (!aad.empty() && aadBuffer) {
            std::memcpy(aadBuffer->getVirtualAddress(), aad.data(), aad.size());
        }
        
        // Configure FPGA registers for the operation
        {
            std::lock_guard<std::mutex> lock(m_regMutex);
            
            // Select crypto module
            if (!writeRegister(REG_MODULE_SELECT, MODULE_CRYPTO)) {
                throw std::runtime_error("Failed to select crypto module");
            }
            
            // Set operation type
            if (operationType == FPGAOperationType::CRYPTO_ENCRYPT) {
                if (!writeRegister(REG_OP_TYPE, OP_CRYPTO_ENCRYPT)) {
                    throw std::runtime_error("Failed to set operation type");
                }
            } else {
                if (!writeRegister(REG_OP_TYPE, OP_CRYPTO_DECRYPT)) {
                    throw std::runtime_error("Failed to set operation type");
                }
            }
            
            // Configure buffer addresses and sizes
            if (!writeRegister(REG_DATA_ADDR, inputBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_DATA_SIZE, inputData.size()) ||
                !writeRegister(REG_KEY_ADDR, keyBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_KEY_SIZE, key.size()) ||
                !writeRegister(REG_NONCE_ADDR, nonceBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_RESULT_ADDR, outputBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_RESULT_SIZE, outputBuffer->getSize())) {
                throw std::runtime_error("Failed to configure buffer addresses");
            }
            
            // Configure AAD if present
            if (!aad.empty() && aadBuffer) {
                if (!writeRegister(REG_AAD_ADDR, aadBuffer->getPhysicalAddress()) ||
                    !writeRegister(REG_AAD_SIZE, aad.size())) {
                    throw std::runtime_error("Failed to configure AAD buffer");
                }
            } else {
                if (!writeRegister(REG_AAD_SIZE, 0)) {
                    throw std::runtime_error("Failed to clear AAD size");
                }
            }
            
            // Start the operation
            if (!writeRegister(REG_CONTROL, CTRL_START)) {
                throw std::runtime_error("Failed to start operation");
            }
            
            // Wait for completion or timeout
            uint32_t status = 0;
            auto startWait = std::chrono::high_resolution_clock::now();
            bool timeout = false;
            
            while (true) {
                if (!readRegister(REG_STATUS, status)) {
                    throw std::runtime_error("Failed to read status register");
                }
                
                if (status & STATUS_DONE) {
                    break;
                }
                
                if (status & STATUS_ERROR) {
                    throw std::runtime_error("FPGA reported error during operation");
                }
                
                auto currentTime = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startWait).count();
                
                if (elapsed > FPGA_OPERATION_TIMEOUT_MS) {
                    timeout = true;
                    break;
                }
                
                // Brief sleep to avoid hammering the bus
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            
            if (timeout) {
                throw std::runtime_error("Operation timed out");
            }
            
            // Get processing time from FPGA
            uint32_t processingTime = 0;
            if (!readRegister(REG_PROCESSING_TIME, processingTime)) {
                throw std::runtime_error("Failed to read processing time");
            }
            
            // Check if there was an error
            uint32_t errorCode = 0;
            if (status & STATUS_ERROR) {
                if (!readRegister(REG_ERROR_CODE, errorCode)) {
                    throw std::runtime_error("Failed to read error code");
                }
                throw std::runtime_error("FPGA operation failed with error code: " + 
                                       std::to_string(errorCode));
            }
            
            // Get result size
            uint32_t resultSize = 0;
            if (!readRegister(REG_RESULT_SIZE, resultSize)) {
                throw std::runtime_error("Failed to read result size");
            }
            
            // Copy result data
            result.data.resize(resultSize);
            std::memcpy(result.data.data(), outputBuffer->getVirtualAddress(), resultSize);
            
            // Set result properties
            result.bytesProcessed = inputData.size();
            result.processingTimeMs = processingTime / 1000.0;  // Convert microseconds to ms
        }
        
        // Update statistics
        updateStats(result.bytesProcessed, result.processingTimeMs);
        
    } catch (const std::exception& e) {
        // Handle any errors
        result.success = false;
        result.errorMessage = e.what();
        
        // Calculate processing time even for failed operations
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        result.processingTimeMs = duration.count();
    }
    
    // Call callback if provided
    if (callback) {
        callback(result);
    }
    
    return result;
}

FPGAOperationResult FPGAInterface::executeCompressionOperation(
    FPGAOperationType operationType,
    const std::vector<uint8_t>& inputData,
    std::function<void(const FPGAOperationResult&)> callback) {
    
    // Validate parameters
    if (inputData.empty()) {
        FPGAOperationResult result;
        result.success = false;
        result.errorCode = 1;
        result.errorMessage = "Invalid parameters: empty input data";
        return result;
    }
    
    // Record start time for performance measurement
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Use simulation if in simulation mode or not connected
    if (m_simulationMode || !m_connected.load()) {
        auto result = simulateCompressionOperation(operationType, inputData);
        
        // Record end time and calculate duration
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        result.processingTimeMs = duration.count();
        
        // Update statistics
        updateStats(result.bytesProcessed, result.processingTimeMs);
        
        // Call callback if provided
        if (callback) {
            callback(result);
        }
        
        return result;
    }
    
    // Real FPGA implementation
    FPGAOperationResult result;
    result.success = true;
    
    try {
        // Allocate DMA buffers for data transfer
        auto inputBuffer = m_dmaController->allocateBuffer(inputData.size());
        
        // For compression, worst case is input size + some overhead
        // For decompression, worst case is potentially much larger than input
        size_t outputSize = (operationType == FPGAOperationType::COMPRESSION) ?
                            inputData.size() + 1024 :  // Add overhead for compression metadata
                            inputData.size() * 4;      // Assume up to 4:1 compression ratio
        
        auto outputBuffer = m_dmaController->allocateBuffer(outputSize);
        
        // Copy data to input buffer
        std::memcpy(inputBuffer->getVirtualAddress(), inputData.data(), inputData.size());
        
        // Configure FPGA registers for the operation
        {
            std::lock_guard<std::mutex> lock(m_regMutex);
            
            // Select compression module
            if (!writeRegister(REG_MODULE_SELECT, MODULE_COMPRESSION)) {
                throw std::runtime_error("Failed to select compression module");
            }
            
            // Set operation type
            if (operationType == FPGAOperationType::COMPRESSION) {
                if (!writeRegister(REG_OP_TYPE, OP_COMPRESSION)) {
                    throw std::runtime_error("Failed to set operation type");
                }
            } else {
                if (!writeRegister(REG_OP_TYPE, OP_DECOMPRESSION)) {
                    throw std::runtime_error("Failed to set operation type");
                }
            }
            
            // Configure buffer addresses and sizes
            if (!writeRegister(REG_DATA_ADDR, inputBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_DATA_SIZE, inputData.size()) ||
                !writeRegister(REG_RESULT_ADDR, outputBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_RESULT_SIZE, outputBuffer->getSize())) {
                throw std::runtime_error("Failed to configure buffer addresses");
            }
            
            // Start the operation
            if (!writeRegister(REG_CONTROL, CTRL_START)) {
                throw std::runtime_error("Failed to start operation");
            }
            
            // Wait for completion or timeout
            uint32_t status = 0;
            auto startWait = std::chrono::high_resolution_clock::now();
            bool timeout = false;
            
            while (true) {
                if (!readRegister(REG_STATUS, status)) {
                    throw std::runtime_error("Failed to read status register");
                }
                
                if (status & STATUS_DONE) {
                    break;
                }
                
                if (status & STATUS_ERROR) {
                    throw std::runtime_error("FPGA reported error during operation");
                }
                
                auto currentTime = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startWait).count();
                
                if (elapsed > FPGA_OPERATION_TIMEOUT_MS) {
                    timeout = true;
                    break;
                }
                
                // Brief sleep to avoid hammering the bus
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            
            if (timeout) {
                throw std::runtime_error("Operation timed out");
            }
            
            // Get processing time from FPGA
            uint32_t processingTime = 0;
            if (!readRegister(REG_PROCESSING_TIME, processingTime)) {
                throw std::runtime_error("Failed to read processing time");
            }
            
            // Check if there was an error
            uint32_t errorCode = 0;
            if (status & STATUS_ERROR) {
                if (!readRegister(REG_ERROR_CODE, errorCode)) {
                    throw std::runtime_error("Failed to read error code");
                }
                throw std::runtime_error("FPGA operation failed with error code: " + 
                                       std::to_string(errorCode));
            }
            
            // Get result size
            uint32_t resultSize = 0;
            if (!readRegister(REG_RESULT_SIZE, resultSize)) {
                throw std::runtime_error("Failed to read result size");
            }
            
            // Copy result data
            result.data.resize(resultSize);
            std::memcpy(result.data.data(), outputBuffer->getVirtualAddress(), resultSize);
            
            // Set result properties
            result.bytesProcessed = inputData.size();
            result.processingTimeMs = processingTime / 1000.0;  // Convert microseconds to ms
        }
        
        // Update statistics
        updateStats(result.bytesProcessed, result.processingTimeMs);
        
    } catch (const std::exception& e) {
        // Handle any errors
        result.success = false;
        result.errorMessage = e.what();
        
        // Calculate processing time even for failed operations
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        result.processingTimeMs = duration.count();
    }
    
    // Call callback if provided
    if (callback) {
        callback(result);
    }
    
    return result;
}

FPGAOperationResult FPGAInterface::executePacketOperation(
    FPGAOperationType operationType,
    const std::vector<uint8_t>& inputData,
    uint64_t connectionID,
    uint32_t packetNumber,
    std::function<void(const FPGAOperationResult&)> callback) {
    
    // Validate parameters
    if (operationType != FPGAOperationType::PACKET_ACK_PROCESSING && inputData.empty()) {
        FPGAOperationResult result;
        result.success = false;
        result.errorCode = 1;
        result.errorMessage = "Invalid parameters: empty input data for non-ACK operation";
        return result;
    }
    
    // Record start time for performance measurement
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Use simulation if in simulation mode or not connected
    if (m_simulationMode || !m_connected.load()) {
        auto result = simulatePacketOperation(operationType, inputData, connectionID, packetNumber);
        
        // Record end time and calculate duration
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        result.processingTimeMs = duration.count();
        
        // Update statistics
        updateStats(result.bytesProcessed, result.processingTimeMs);
        
        // Call callback if provided
        if (callback) {
            callback(result);
        }
        
        return result;
    }
    
    // Real FPGA implementation
    FPGAOperationResult result;
    result.success = true;
    
    try {
        // For ACK processing, we might not have input data
        auto inputBuffer = inputData.empty() ? nullptr : 
                          m_dmaController->allocateBuffer(inputData.size());
        
        // Output buffer size depends on operation type
        size_t outputSize = 0;
        
        switch (operationType) {
            case FPGAOperationType::PACKET_FRAMING:
                // Framing adds headers, so allocate more space
                outputSize = inputData.size() + 256;
                break;
                
            case FPGAOperationType::PACKET_ACK_PROCESSING:
                // ACK processing typically returns a small ACK frame
                outputSize = 128;
                break;
                
            case FPGAOperationType::PACKET_RETRANSMISSION:
                // Retransmission returns the original packet plus headers
                outputSize = inputData.size() + 256;
                break;
                
            default:
                throw std::runtime_error("Unsupported packet operation type");
        }
        
        auto outputBuffer = m_dmaController->allocateBuffer(outputSize);
        
        // Copy input data if we have any
        if (!inputData.empty() && inputBuffer) {
            std::memcpy(inputBuffer->getVirtualAddress(), inputData.data(), inputData.size());
        }
        
        // Configure FPGA registers for the operation
        {
            std::lock_guard<std::mutex> lock(m_regMutex);
            
            // Select packet processor module
            if (!writeRegister(REG_MODULE_SELECT, MODULE_PACKET)) {
                throw std::runtime_error("Failed to select packet processor module");
            }
            
            // Set operation type
            uint32_t opTypeReg = 0;
            switch (operationType) {
                case FPGAOperationType::PACKET_FRAMING:
                    opTypeReg = OP_PACKET_FRAME;
                    break;
                case FPGAOperationType::PACKET_ACK_PROCESSING:
                    opTypeReg = OP_PACKET_ACK;
                    break;
                case FPGAOperationType::PACKET_RETRANSMISSION:
                    opTypeReg = OP_PACKET_RETRANSMIT;
                    break;
                default:
                    throw std::runtime_error("Unsupported packet operation type");
            }
            
            if (!writeRegister(REG_OP_TYPE, opTypeReg)) {
                throw std::runtime_error("Failed to set operation type");
            }
            
            // Set connection ID and packet number
            if (!writeRegister(REG_CONNECTION_ID_HIGH, connectionID >> 32) ||
                !writeRegister(REG_CONNECTION_ID_LOW, connectionID & 0xFFFFFFFF) ||
                !writeRegister(REG_PACKET_NUMBER, packetNumber)) {
                throw std::runtime_error("Failed to set connection parameters");
            }
            
            // Configure buffer addresses and sizes
            if (inputBuffer) {
                if (!writeRegister(REG_DATA_ADDR, inputBuffer->getPhysicalAddress()) ||
                    !writeRegister(REG_DATA_SIZE, inputData.size())) {
                    throw std::runtime_error("Failed to configure input buffer");
                }
            } else {
                // No input data (e.g., for ACK processing)
                if (!writeRegister(REG_DATA_SIZE, 0)) {
                    throw std::runtime_error("Failed to set zero input size");
                }
            }
            
            if (!writeRegister(REG_RESULT_ADDR, outputBuffer->getPhysicalAddress()) ||
                !writeRegister(REG_RESULT_SIZE, outputBuffer->getSize())) {
                throw std::runtime_error("Failed to configure output buffer");
            }
            
            // Start the operation
            if (!writeRegister(REG_CONTROL, CTRL_START)) {
                throw std::runtime_error("Failed to start operation");
            }
            
            // Wait for completion or timeout
            uint32_t status = 0;
            auto startWait = std::chrono::high_resolution_clock::now();
            bool timeout = false;
            
            while (true) {
                if (!readRegister(REG_STATUS, status)) {
                    throw std::runtime_error("Failed to read status register");
                }
                
                if (status & STATUS_DONE) {
                    break;
                }
                
                if (status & STATUS_ERROR) {
                    throw std::runtime_error("FPGA reported error during operation");
                }
                
                auto currentTime = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startWait).count();
                
                if (elapsed > FPGA_OPERATION_TIMEOUT_MS) {
                    timeout = true;
                    break;
                }
                
                // Brief sleep to avoid hammering the bus
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            
            if (timeout) {
                throw std::runtime_error("Operation timed out");
            }
            
            // Get processing time from FPGA
            uint32_t processingTime = 0;
            if (!readRegister(REG_PROCESSING_TIME, processingTime)) {
                throw std::runtime_error("Failed to read processing time");
            }
            
            // Check if there was an error
            uint32_t errorCode = 0;
            if (status & STATUS_ERROR) {
                if (!readRegister(REG_ERROR_CODE, errorCode)) {
                    throw std::runtime_error("Failed to read error code");
                }
                throw std::runtime_error("FPGA operation failed with error code: " + 
                                       std::to_string(errorCode));
            }
            
            // Get result size
            uint32_t resultSize = 0;
            if (!readRegister(REG_RESULT_SIZE, resultSize)) {
                throw std::runtime_error("Failed to read result size");
            }
            
            // Copy result data
            result.data.resize(resultSize);
            std::memcpy(result.data.data(), outputBuffer->getVirtualAddress(), resultSize);
            
            // Set result properties
            result.bytesProcessed = inputData.size();
            result.processingTimeMs = processingTime / 1000.0;  // Convert microseconds to ms
        }
        
        // Update statistics
        updateStats(result.bytesProcessed, result.processingTimeMs);
        
    } catch (const std::exception& e) {
        // Handle any errors
        result.success = false;
        result.errorMessage = e.what();
        
        // Calculate processing time even for failed operations
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        result.processingTimeMs = duration.count();
    }
    
    // Call callback if provided
    if (callback) {
        callback(result);
    }
    
    return result;
}

bool FPGAInterface::resetFPGA() {
    if (m_simulationMode) {
        return true;
    }
    
    std::lock_guard<std::mutex> lock(m_regMutex);
    return writeRegister(REG_CONTROL, CTRL_RESET);
}

void FPGAInterface::printStats() const {
    std::cout << "FPGA Acceleration Statistics:" << std::endl;
    std::cout << "  Crypto operations: " << m_cryptoOpsCount.load() << std::endl;
    std::cout << "  Compression operations: " << m_compressionOpsCount.load() << std::endl;
    std::cout << "  Packet operations: " << m_packetOpsCount.load() << std::endl;
    std::cout << "  Total bytes processed: " << m_totalBytesProcessed.load() << std::endl;
    
    // Calculate average processing time
    double avgTime = 0;
    uint64_t totalOps = m_cryptoOpsCount.load() + m_compressionOpsCount.load() + m_packetOpsCount.load();
    if (totalOps > 0) {
        avgTime = m_totalProcessingTimeMs.load() / totalOps;
    }
    
    std::cout << "  Average processing time: " << std::fixed << std::setprecision(2) 
              << avgTime << " ms" << std::endl;
}

bool FPGAInterface::openDevice() {
    if (m_deviceFd >= 0) {
        return true;  // Already open
    }
    
    m_deviceFd = open(m_devicePath.c_str(), O_RDWR);
    if (m_deviceFd < 0) {
        std::cerr << "Failed to open FPGA device " << m_devicePath << ": " 
                  << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

void FPGAInterface::closeDevice() {
    if (m_deviceFd >= 0) {
        close(m_deviceFd);
        m_deviceFd = -1;
    }
}

bool FPGAInterface::writeRegister(uint32_t regAddr, uint32_t value) {
    if (m_deviceFd < 0) {
        return false;
    }
    
    // Device-specific ioctl for register write
    struct {
        uint32_t address;
        uint32_t value;
    } regWrite;
    
    regWrite.address = regAddr;
    regWrite.value = value;
    
    // Using a placeholder ioctl code - would need actual driver ioctl code
    const int IOCTL_WRITE_REGISTER = 0x1000;
    if (ioctl(m_deviceFd, IOCTL_WRITE_REGISTER, &regWrite) < 0) {
        std::cerr << "Failed to write register 0x" << std::hex << regAddr 
                  << ": " << std::dec << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

bool FPGAInterface::readRegister(uint32_t regAddr, uint32_t& value) const {
    if (m_deviceFd < 0) {
        return false;
    }
    
    // Device-specific ioctl for register read
    struct {
        uint32_t address;
        uint32_t value;
    } regRead;
    
    regRead.address = regAddr;
    regRead.value = 0;
    
    // Using a placeholder ioctl code - would need actual driver ioctl code
    const int IOCTL_READ_REGISTER = 0x1001;
    if (ioctl(m_deviceFd, IOCTL_READ_REGISTER, &regRead) < 0) {
        std::cerr << "Failed to read register 0x" << std::hex << regAddr 
                  << ": " << std::dec << strerror(errno) << std::endl;
        return false;
    }
    
    value = regRead.value;
    return true;
}

void FPGAInterface::updateStats(uint32_t bytesProcessed, double processingTimeMs) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_totalBytesProcessed += bytesProcessed;
    m_totalProcessingTimeMs += processingTimeMs;
}

FPGAOperationResult FPGAInterface::simulateCryptoOperation(
    FPGAOperationType operationType,
    const std::vector<uint8_t>& inputData,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& nonce,
    const std::vector<uint8_t>& aad) {
    
    FPGAOperationResult result;
    result.success = true;
    result.bytesProcessed = inputData.size();
    
    // Simulate processing delay based on data size (approximately 10MB/s throughput)
    double processingDelayMs = inputData.size() / 10000.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(processingDelayMs)));
    
    // Use OpenSSL for actual crypto in simulation mode
    EVP_CIPHER_CTX *ctx;
    int len;
    int ciphertext_len;
    
    // Create and initialize the context
    if (!(ctx = EVP_CIPHER_CTX_new())) {
        result.success = false;
        result.errorMessage = "Failed to create OpenSSL cipher context";
        return result;
    }
    
    try {
        // Determine key size and select appropriate cipher
        const EVP_CIPHER* cipher = nullptr;
        switch (key.size()) {
            case 16:  // 128-bit key
                cipher = EVP_aes_128_gcm();
                break;
            case 24:  // 192-bit key
                cipher = EVP_aes_192_gcm();
                break;
            case 32:  // 256-bit key
                cipher = EVP_aes_256_gcm();
                break;
            default:
                throw std::runtime_error("Unsupported key size");
        }
        
        // Initialize the encryption/decryption operation
        if (operationType == FPGAOperationType::CRYPTO_ENCRYPT) {
            if (1 != EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr)) {
                throw std::runtime_error("Failed to initialize encryption");
            }
        } else {
            if (1 != EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr)) {
                throw std::runtime_error("Failed to initialize decryption");
            }
        }
        
        // Set IV length (for GCM, this is fixed at 12 bytes / 96 bits)
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr)) {
            throw std::runtime_error("Failed to set IV length");
        }
        
        // Initialize key and IV
        if (operationType == FPGAOperationType::CRYPTO_ENCRYPT) {
            if (1 != EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data())) {
                throw std::runtime_error("Failed to set encryption key and IV");
            }
        } else {
            if (1 != EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data())) {
                throw std::runtime_error("Failed to set decryption key and IV");
            }
        }
        
        // Provide AAD data if available
        if (!aad.empty()) {
            if (operationType == FPGAOperationType::CRYPTO_ENCRYPT) {
                if (1 != EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), aad.size())) {
                    throw std::runtime_error("Failed to set AAD for encryption");
                }
            } else {
                if (1 != EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(), aad.size())) {
                    throw std::runtime_error("Failed to set AAD for decryption");
                }
            }
        }
        
        // Resize result buffer to accommodate output data and tag
        result.data.resize(inputData.size() + 16);  // +16 for GCM tag
        
        // Perform encryption/decryption
        if (operationType == FPGAOperationType::CRYPTO_ENCRYPT) {
            if (1 != EVP_EncryptUpdate(ctx, result.data.data(), &len, 
                                     inputData.data(), inputData.size())) {
                throw std::runtime_error("Failed during encryption");
            }
            ciphertext_len = len;
            
            // Finalize encryption and get the tag
            if (1 != EVP_EncryptFinal_ex(ctx, result.data.data() + len, &len)) {
                throw std::runtime_error("Failed to finalize encryption");
            }
            ciphertext_len += len;
            
            // Get the tag
            if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, 
                                        result.data.data() + ciphertext_len)) {
                throw std::runtime_error("Failed to get authentication tag");
            }
            ciphertext_len += 16;  // Add tag length
            
        } else {
            // For decryption, we need to separate the tag from the ciphertext
            std::vector<uint8_t> tag(16);
            
            // If input contains the tag (last 16 bytes), extract it
            if (inputData.size() >= 16) {
                std::copy(inputData.end() - 16, inputData.end(), tag.begin());
                
                // Set the expected tag value
                if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data())) {
                    throw std::runtime_error("Failed to set authentication tag");
                }
                
                // Process the ciphertext (excluding the tag)
                if (1 != EVP_DecryptUpdate(ctx, result.data.data(), &len, 
                                         inputData.data(), inputData.size() - 16)) {
                    throw std::runtime_error("Failed during decryption");
                }
                ciphertext_len = len;
                
                // Finalize decryption - this will verify the tag
                int ret = EVP_DecryptFinal_ex(ctx, result.data.data() + len, &len);
                if (ret <= 0) {
                    throw std::runtime_error("Authentication failed during decryption");
                }
                ciphertext_len += len;
                
            } else {
                throw std::runtime_error("Input data too small for decryption with tag");
            }
        }
        
        // Resize the output data to the actual size
        result.data.resize(ciphertext_len);
        
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }
    
    // Clean up
    EVP_CIPHER_CTX_free(ctx);
    
    // Increment crypto operations counter
    m_cryptoOpsCount++;
    
    return result;
}

FPGAOperationResult FPGAInterface::simulateCompressionOperation(
    FPGAOperationType operationType,
    const std::vector<uint8_t>& inputData) {
    
    FPGAOperationResult result;
    result.success = true;
    result.bytesProcessed = inputData.size();
    
    // Simulate processing delay based on data size (approximately 20MB/s throughput)
    double processingDelayMs = inputData.size() / 20000.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(processingDelayMs)));
    
    // Very simple simulated compression/decompression
    // Just to provide some bytes for testing
    if (operationType == FPGAOperationType::COMPRESSION) {
        // "Compress" by inserting a simple header and removing some repetition
        // This is NOT real compression, just a simulation
        
        // Header: 4-byte magic number + 4-byte original size
        std::vector<uint8_t> header = {0x51, 0x43, 0x4D, 0x50};  // "QCMP"
        
        // Add original size as little-endian 32-bit value
        uint32_t size = inputData.size();
        header.push_back(size & 0xFF);
        header.push_back((size >> 8) & 0xFF);
        header.push_back((size >> 16) & 0xFF);
        header.push_back((size >> 24) & 0xFF);
        
        // Initialize result with header
        result.data = header;
        
        // Simple RLE-like encoding for sequences of repeated bytes
        for (size_t i = 0; i < inputData.size(); i++) {
            uint8_t currentByte = inputData[i];
            size_t count = 1;
            
            // Count consecutive identical bytes
            while (i + count < inputData.size() && inputData[i + count] == currentByte && count < 255) {
                count++;
            }
            
            if (count >= 4) {
                // Encode repeats: marker byte (0xFF), count, value
                result.data.push_back(0xFF);
                result.data.push_back(count);
                result.data.push_back(currentByte);
                i += count - 1;  // Skip the repeated bytes
            } else {
                // Copy literal bytes
                for (size_t j = 0; j < count; j++) {
                    result.data.push_back(currentByte);
                }
                i += count - 1;  // Skip the processed bytes
            }
        }
        
    } else {  // Decompression
        // Check for valid header
        if (inputData.size() < 8 || 
            inputData[0] != 0x51 || inputData[1] != 0x43 || 
            inputData[2] != 0x4D || inputData[3] != 0x50) {
            result.success = false;
            result.errorMessage = "Invalid compression format";
            return result;
        }
        
        // Extract original size
        uint32_t originalSize = 
            inputData[4] | (inputData[5] << 8) | (inputData[6] << 16) | (inputData[7] << 24);
        
        // Pre-allocate result buffer
        result.data.reserve(originalSize);
        
        // Decode compressed data
        for (size_t i = 8; i < inputData.size(); i++) {
            if (inputData[i] == 0xFF && i + 2 < inputData.size()) {
                // Repeat sequence
                uint8_t count = inputData[i + 1];
                uint8_t value = inputData[i + 2];
                
                for (int j = 0; j < count; j++) {
                    result.data.push_back(value);
                }
                
                i += 2;  // Skip count and value bytes
            } else {
                // Literal byte
                result.data.push_back(inputData[i]);
            }
        }
    }
    
    // Increment compression operations counter
    m_compressionOpsCount++;
    
    return result;
}

FPGAOperationResult FPGAInterface::simulatePacketOperation(
    FPGAOperationType operationType,
    const std::vector<uint8_t>& inputData,
    uint64_t connectionID,
    uint32_t packetNumber) {
    
    FPGAOperationResult result;
    result.success = true;
    result.bytesProcessed = inputData.size();
    
    // Simulate processing delay based on operation type
    int processingDelayMs = 1;  // Default minimal delay
    
    switch (operationType) {
        case FPGAOperationType::PACKET_FRAMING:
            processingDelayMs = 2 + inputData.size() / 50000;  // ~50MB/s
            break;
            
        case FPGAOperationType::PACKET_ACK_PROCESSING:
            processingDelayMs = 1;  // ACK processing is fast
            break;
            
        case FPGAOperationType::PACKET_RETRANSMISSION:
            processingDelayMs = 2 + inputData.size() / 50000;  // ~50MB/s
            break;
            
        default:
            result.success = false;
            result.errorMessage = "Unsupported packet operation";
            return result;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(processingDelayMs));
    
    // Simulate different packet operations
    if (operationType == FPGAOperationType::PACKET_FRAMING) {
        // Create a simplified QUIC packet with headers
        
        // Initial byte - use 0xC0 for 1-RTT packet
        std::vector<uint8_t> header = {0xC0};
        
        // Add connection ID (truncated to 8 bytes in this simulation)
        for (int i = 0; i < 8; i++) {
            header.push_back((connectionID >> (i * 8)) & 0xFF);
        }
        
        // Add packet number (4 bytes)
        header.push_back(packetNumber & 0xFF);
        header.push_back((packetNumber >> 8) & 0xFF);
        header.push_back((packetNumber >> 16) & 0xFF);
        header.push_back((packetNumber >> 24) & 0xFF);
        
        // Add a simple STREAM frame header
        header.push_back(0x08);  // STREAM frame type
        header.push_back(0x00);  // Stream ID (0)
        
        // Add length field (2 bytes, little endian)
        uint16_t length = inputData.size();
        header.push_back(length & 0xFF);
        header.push_back((length >> 8) & 0xFF);
        
        // Combine header and data
        result.data = header;
        result.data.insert(result.data.end(), inputData.begin(), inputData.end());
        
    } else if (operationType == FPGAOperationType::PACKET_ACK_PROCESSING) {
        // Create a simple ACK frame
        std::vector<uint8_t> ackFrame = {0x02};  // ACK frame type
        
        // Largest acknowledged (4 bytes)
        ackFrame.push_back(packetNumber & 0xFF);
        ackFrame.push_back((packetNumber >> 8) & 0xFF);
        ackFrame.push_back((packetNumber >> 16) & 0xFF);
        ackFrame.push_back((packetNumber >> 24) & 0xFF);
        
        // ACK delay (2 bytes, simulated value)
        uint16_t ackDelay = 10;  // 10ms in QUIC's time units
        ackFrame.push_back(ackDelay & 0xFF);
        ackFrame.push_back((ackDelay >> 8) & 0xFF);
        
        // ACK range count (1 byte)
        ackFrame.push_back(0x00);  // No additional ranges
        
        // First ACK range (1 byte)
        ackFrame.push_back(0x00);  // Only acknowledging the latest packet
        
        result.data = ackFrame;
        
    } else if (operationType == FPGAOperationType::PACKET_RETRANSMISSION) {
        // For retransmission, we simply reframe the packet with updated headers
        // Similar to PACKET_FRAMING but with retransmission flags
        
        // Initial byte - use 0xC0 for 1-RTT packet
        std::vector<uint8_t> header = {0xC0};
        
        // Add connection ID (truncated to 8 bytes in this simulation)
        for (int i = 0; i < 8; i++) {
            header.push_back((connectionID >> (i * 8)) & 0xFF);
        }
        
        // Add packet number (4 bytes) - for retransmission, use new packet number
        uint32_t newPacketNumber = packetNumber + 1;
        header.push_back(newPacketNumber & 0xFF);
        header.push_back((newPacketNumber >> 8) & 0xFF);
        header.push_back((newPacketNumber >> 16) & 0xFF);
        header.push_back((newPacketNumber >> 24) & 0xFF);
        
        // Add a simple STREAM frame header
        header.push_back(0x08);  // STREAM frame type
        header.push_back(0x00);  // Stream ID (0)
        
        // Add length field (2 bytes, little endian)
        uint16_t length = inputData.size();
        header.push_back(length & 0xFF);
        header.push_back((length >> 8) & 0xFF);
        
        // Combine header and data
        result.data = header;
        result.data.insert(result.data.end(), inputData.begin(), inputData.end());
    }
    
    // Increment packet operations counter
    m_packetOpsCount++;
    
    return result;
}
