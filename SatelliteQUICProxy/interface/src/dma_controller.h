/**
 * @file dma_controller.h
 * @brief DMA controller for efficient data transfer between host and FPGA
 * 
 * This class provides a high-level interface for managing DMA transfers between
 * the host system and the FPGA, handling buffer allocation, setup, and transfer
 * synchronization.
 */

#ifndef DMA_CONTROLLER_H
#define DMA_CONTROLLER_H

#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include "buffer_manager.h"

namespace DMA {

/**
 * @enum TransferDirection
 * @brief Direction of DMA transfer
 */
enum class TransferDirection {
    HOST_TO_DEVICE,  // Transfer from host memory to FPGA
    DEVICE_TO_HOST   // Transfer from FPGA to host memory
};

/**
 * @enum TransferType
 * @brief Type of DMA transfer
 */
enum class TransferType {
    BLOCKING,       // Synchronous, blocking transfer
    NON_BLOCKING    // Asynchronous, non-blocking transfer
};

/**
 * @struct TransferResult
 * @brief Result of a DMA transfer operation
 */
struct TransferResult {
    bool success;                  // Did the transfer complete successfully?
    uint32_t bytesTransferred;     // Number of bytes transferred
    uint32_t errorCode;            // Error code if transfer failed
    std::string errorMessage;      // Error message if transfer failed
    
    TransferResult() : success(false), bytesTransferred(0), errorCode(0) {}
};

/**
 * @class Controller
 * @brief Main class for managing DMA transfers between host and FPGA
 */
class Controller {
public:
    /**
     * Constructor
     * 
     * @param deviceFd File descriptor for the FPGA device
     */
    Controller(int deviceFd);
    
    /**
     * Destructor ensures proper cleanup of DMA resources
     */
    ~Controller();
    
    /**
     * Initialize the DMA controller
     * 
     * @return true if initialization successful
     */
    bool initialize();
    
    /**
     * Allocate a DMA buffer for data transfer
     * 
     * @param size Size of the buffer in bytes
     * @return Shared pointer to the allocated buffer or nullptr on failure
     */
    std::shared_ptr<Buffer> allocateBuffer(size_t size);
    
    /**
     * Perform a DMA transfer
     * 
     * @param buffer Buffer containing data to transfer or to receive data
     * @param size Number of bytes to transfer
     * @param direction Direction of the transfer
     * @param type Blocking or non-blocking transfer
     * @param callback Callback function for non-blocking transfers
     * @return Result of the transfer operation
     */
    TransferResult transfer(
        std::shared_ptr<Buffer> buffer,
        size_t size,
        TransferDirection direction,
        TransferType type = TransferType::BLOCKING,
        std::function<void(const TransferResult&)> callback = nullptr
    );
    
    /**
     * Wait for a previously initiated non-blocking transfer to complete
     * 
     * @param buffer Buffer associated with the transfer
     * @param timeout Timeout in milliseconds, 0 for infinite
     * @return Result of the transfer operation
     */
    TransferResult waitForTransfer(
        std::shared_ptr<Buffer> buffer,
        uint32_t timeout = 0
    );
    
    /**
     * Check if a previously initiated non-blocking transfer has completed
     * 
     * @param buffer Buffer associated with the transfer
     * @return true if the transfer is complete
     */
    bool isTransferComplete(std::shared_ptr<Buffer> buffer);
    
    /**
     * Synchronize the buffer with the device
     * This ensures any pending writes are flushed to the device or any
     * pending reads are visible to the host
     * 
     * @param buffer Buffer to synchronize
     * @param direction Direction to synchronize (to or from device)
     * @return true if synchronization successful
     */
    bool synchronizeBuffer(
        std::shared_ptr<Buffer> buffer,
        TransferDirection direction
    );

private:
    // Device file descriptor
    int m_deviceFd;
    
    // Buffer manager for DMA buffers
    std::unique_ptr<BufferManager> m_bufferManager;
    
    // Tracking pending transfers
    struct PendingTransfer {
        std::shared_ptr<Buffer> buffer;
        size_t size;
        TransferDirection direction;
        std::function<void(const TransferResult&)> callback;
        bool completed;
        TransferResult result;
    };
    
    std::vector<std::unique_ptr<PendingTransfer>> m_pendingTransfers;
    std::mutex m_transfersMutex;
    
    // DMA engine control
    struct DMAEngine {
        uint64_t baseAddress;      // Base address of the DMA engine registers
        uint32_t channelCount;     // Number of DMA channels
        std::atomic<bool> busy;    // Is the engine currently in use?
        
        DMAEngine() : baseAddress(0), channelCount(0), busy(false) {}
    };
    
    std::vector<DMAEngine> m_dmaEngines;
    
    // Private helper methods
    bool setupDMAEngines();
    bool startDMATransfer(std::shared_ptr<Buffer> buffer, size_t size, 
                         TransferDirection direction, uint32_t channelId);
    bool checkDMATransferComplete(uint32_t channelId, uint32_t& bytesTransferred);
    bool abortDMATransfer(uint32_t channelId);
    DMAEngine* getAvailableDMAEngine();
};

} // namespace DMA

#endif // DMA_CONTROLLER_H
