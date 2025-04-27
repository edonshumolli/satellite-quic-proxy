/**
 * @file dma_controller.cpp
 * @brief Implementation of the DMA controller for FPGA data transfer
 */

#include "dma_controller.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>

// FPGA DMA controller register offsets
#define DMA_REG_CONTROL           0x00  // Control register
#define DMA_REG_STATUS            0x04  // Status register
#define DMA_REG_SRC_ADDR_LOW      0x08  // Source address low 32 bits
#define DMA_REG_SRC_ADDR_HIGH     0x0C  // Source address high 32 bits
#define DMA_REG_DST_ADDR_LOW      0x10  // Destination address low 32 bits
#define DMA_REG_DST_ADDR_HIGH     0x14  // Destination address high 32 bits
#define DMA_REG_TRANSFER_SIZE     0x18  // Transfer size in bytes
#define DMA_REG_TRANSFER_ID       0x1C  // Transfer ID
#define DMA_REG_TRANSFERRED_BYTES 0x20  // Number of bytes transferred
#define DMA_REG_ERROR_CODE        0x24  // Error code

// DMA control register bit definitions
#define DMA_CTRL_START            0x00000001  // Start transfer
#define DMA_CTRL_ABORT            0x00000002  // Abort transfer
#define DMA_CTRL_RESET            0x00000004  // Reset engine
#define DMA_CTRL_IRQ_EN           0x00000008  // Enable interrupts
#define DMA_CTRL_DIR_H2D          0x00000000  // Direction: Host to Device
#define DMA_CTRL_DIR_D2H          0x00000010  // Direction: Device to Host
#define DMA_CTRL_SYNC             0x00000020  // Synchronize after transfer

// DMA status register bit definitions
#define DMA_STATUS_BUSY           0x00000001  // Engine busy
#define DMA_STATUS_DONE           0x00000002  // Transfer complete
#define DMA_STATUS_ERROR          0x00000004  // Error occurred
#define DMA_STATUS_IRQ            0x00000008  // Interrupt pending

// Device-specific ioctl commands (would be defined in driver header)
#define IOCTL_GET_DMA_INFO        0x2000
#define IOCTL_MAP_DMA_REGION      0x2001
#define IOCTL_UNMAP_DMA_REGION    0x2002
#define IOCTL_SYNC_DMA_BUFFER     0x2003

// Maximum number of DMA engines
#define MAX_DMA_ENGINES           4

// Minimum and maximum transfer sizes
#define MIN_DMA_TRANSFER_SIZE     64
#define MAX_DMA_TRANSFER_SIZE     (16 * 1024 * 1024)  // 16 MB

namespace DMA {

Controller::Controller(int deviceFd)
    : m_deviceFd(deviceFd),
      m_bufferManager(std::make_unique<BufferManager>(deviceFd)) {
}

Controller::~Controller() {
    // Wait for any pending transfers to complete
    for (const auto& transfer : m_pendingTransfers) {
        if (!transfer->completed) {
            // Find the channel associated with this transfer and abort it
            for (auto& engine : m_dmaEngines) {
                // In a real implementation, we would track which channel is used for which transfer
                // For this simplified version, we'll just attempt to abort all channels
                for (uint32_t channelId = 0; channelId < engine.channelCount; channelId++) {
                    abortDMATransfer(channelId);
                }
            }
        }
    }
    
    // Reset DMA engines
    for (auto& engine : m_dmaEngines) {
        volatile uint32_t* controlReg = reinterpret_cast<volatile uint32_t*>(engine.baseAddress + DMA_REG_CONTROL);
        *controlReg = DMA_CTRL_RESET;
    }
}

bool Controller::initialize() {
    // Initialize buffer manager
    if (!m_bufferManager->initialize()) {
        std::cerr << "Failed to initialize buffer manager" << std::endl;
        return false;
    }
    
    // Set up DMA engines
    if (!setupDMAEngines()) {
        std::cerr << "Failed to set up DMA engines" << std::endl;
        return false;
    }
    
    return true;
}

std::shared_ptr<Buffer> Controller::allocateBuffer(size_t size) {
    if (size == 0 || size > MAX_DMA_TRANSFER_SIZE) {
        std::cerr << "Invalid buffer size request: " << size << std::endl;
        return nullptr;
    }
    
    return m_bufferManager->allocateBuffer(size);
}

TransferResult Controller::transfer(
    std::shared_ptr<Buffer> buffer,
    size_t size,
    TransferDirection direction,
    TransferType type,
    std::function<void(const TransferResult&)> callback) {
    
    // Validate parameters
    if (!buffer || size == 0 || size > buffer->getSize()) {
        TransferResult result;
        result.success = false;
        result.errorMessage = "Invalid buffer or size";
        return result;
    }
    
    if (size < MIN_DMA_TRANSFER_SIZE || size > MAX_DMA_TRANSFER_SIZE) {
        TransferResult result;
        result.success = false;
        result.errorMessage = "Transfer size out of range";
        return result;
    }
    
    // Get an available DMA engine
    DMAEngine* engine = getAvailableDMAEngine();
    if (!engine) {
        TransferResult result;
        result.success = false;
        result.errorMessage = "No available DMA engines";
        return result;
    }
    
    // Mark the engine as busy
    engine->busy.store(true);
    
    // For simplicity, always use channel 0
    uint32_t channelId = 0;
    
    // Start the DMA transfer
    if (!startDMATransfer(buffer, size, direction, channelId)) {
        engine->busy.store(false);
        TransferResult result;
        result.success = false;
        result.errorMessage = "Failed to start DMA transfer";
        return result;
    }
    
    if (type == TransferType::BLOCKING) {
        // Blocking transfer - wait for completion
        bool success = false;
        uint32_t bytesTransferred = 0;
        
        // Poll for completion
        const int pollIntervalUs = 100;  // 100 microseconds
        int timeout = 0;  // No timeout for now
        
        while (true) {
            if (checkDMATransferComplete(channelId, bytesTransferred)) {
                success = true;
                break;
            }
            
            if (timeout > 0 && timeout-- == 0) {
                // Timeout - abort transfer
                abortDMATransfer(channelId);
                break;
            }
            
            // Sleep briefly to avoid hammering the bus
            std::this_thread::sleep_for(std::chrono::microseconds(pollIntervalUs));
        }
        
        // Mark the engine as available
        engine->busy.store(false);
        
        // Return result
        TransferResult result;
        result.success = success;
        result.bytesTransferred = bytesTransferred;
        
        // If transfer was successful, synchronize the buffer
        if (success) {
            synchronizeBuffer(buffer, direction);
        } else {
            // Read error code
            volatile uint32_t* errorReg = reinterpret_cast<volatile uint32_t*>(
                engine->baseAddress + DMA_REG_ERROR_CODE);
            result.errorCode = *errorReg;
            result.errorMessage = "DMA transfer failed";
        }
        
        return result;
        
    } else {
        // Non-blocking transfer - create pending transfer record
        auto pendingTransfer = std::make_unique<PendingTransfer>();
        pendingTransfer->buffer = buffer;
        pendingTransfer->size = size;
        pendingTransfer->direction = direction;
        pendingTransfer->callback = callback;
        pendingTransfer->completed = false;
        
        // Add to pending transfers list
        {
            std::lock_guard<std::mutex> lock(m_transfersMutex);
            m_pendingTransfers.push_back(std::move(pendingTransfer));
        }
        
        // Launch a detached thread to wait for completion
        std::thread([this, channelId, buffer, direction, engine]() {
            uint32_t bytesTransferred = 0;
            bool success = false;
            
            // Poll for completion
            const int pollIntervalUs = 100;  // 100 microseconds
            
            while (true) {
                if (checkDMATransferComplete(channelId, bytesTransferred)) {
                    success = true;
                    break;
                }
                
                // Sleep briefly to avoid hammering the bus
                std::this_thread::sleep_for(std::chrono::microseconds(pollIntervalUs));
            }
            
            // Mark the engine as available
            engine->busy.store(false);
            
            // Find the pending transfer in the list
            std::lock_guard<std::mutex> lock(m_transfersMutex);
            for (auto& transfer : m_pendingTransfers) {
                if (transfer->buffer == buffer && !transfer->completed) {
                    // Mark as completed
                    transfer->completed = true;
                    
                    // Set result
                    transfer->result.success = success;
                    transfer->result.bytesTransferred = bytesTransferred;
                    
                    // If transfer was successful, synchronize the buffer
                    if (success) {
                        synchronizeBuffer(buffer, direction);
                    } else {
                        // Read error code
                        volatile uint32_t* errorReg = reinterpret_cast<volatile uint32_t*>(
                            engine->baseAddress + DMA_REG_ERROR_CODE);
                        transfer->result.errorCode = *errorReg;
                        transfer->result.errorMessage = "DMA transfer failed";
                    }
                    
                    // Call the callback if provided
                    if (transfer->callback) {
                        transfer->callback(transfer->result);
                    }
                    
                    break;
                }
            }
        }).detach();
        
        // Return preliminary result
        TransferResult result;
        result.success = true;  // Indicates transfer was started successfully
        result.bytesTransferred = 0;
        return result;
    }
}

TransferResult Controller::waitForTransfer(
    std::shared_ptr<Buffer> buffer,
    uint32_t timeout) {
    
    if (!buffer) {
        TransferResult result;
        result.success = false;
        result.errorMessage = "Invalid buffer";
        return result;
    }
    
    // Look for the pending transfer
    PendingTransfer* pendingTransfer = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(m_transfersMutex);
        for (auto& transfer : m_pendingTransfers) {
            if (transfer->buffer == buffer) {
                pendingTransfer = transfer.get();
                break;
            }
        }
    }
    
    if (!pendingTransfer) {
        TransferResult result;
        result.success = false;
        result.errorMessage = "No pending transfer found for this buffer";
        return result;
    }
    
    // If already completed, return the result
    if (pendingTransfer->completed) {
        return pendingTransfer->result;
    }
    
    // Wait for completion
    auto startTime = std::chrono::steady_clock::now();
    
    while (!pendingTransfer->completed) {
        // Check if timeout has been reached
        if (timeout > 0) {
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                currentTime - startTime).count();
            
            if (elapsedMs >= timeout) {
                TransferResult result;
                result.success = false;
                result.errorMessage = "Timeout waiting for transfer completion";
                return result;
            }
        }
        
        // Sleep briefly to avoid spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    // Return the result
    return pendingTransfer->result;
}

bool Controller::isTransferComplete(std::shared_ptr<Buffer> buffer) {
    if (!buffer) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_transfersMutex);
    for (auto& transfer : m_pendingTransfers) {
        if (transfer->buffer == buffer) {
            return transfer->completed;
        }
    }
    
    // Transfer not found - could have already been completed and removed
    return true;
}

bool Controller::synchronizeBuffer(
    std::shared_ptr<Buffer> buffer,
    TransferDirection direction) {
    
    if (!buffer) {
        return false;
    }
    
    // Request buffer synchronization via ioctl
    struct {
        void* virtualAddress;
        size_t size;
        int direction;  // 0 for H2D, 1 for D2H
    } syncParams;
    
    syncParams.virtualAddress = buffer->getVirtualAddress();
    syncParams.size = buffer->getSize();
    syncParams.direction = (direction == TransferDirection::HOST_TO_DEVICE) ? 0 : 1;
    
    if (ioctl(m_deviceFd, IOCTL_SYNC_DMA_BUFFER, &syncParams) < 0) {
        std::cerr << "Failed to synchronize DMA buffer: " << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

bool Controller::setupDMAEngines() {
    // Query device for DMA engine information
    struct {
        uint32_t count;
        struct {
            uint64_t baseAddress;
            uint32_t channelCount;
        } engines[MAX_DMA_ENGINES];
    } dmaInfo;
    
    if (ioctl(m_deviceFd, IOCTL_GET_DMA_INFO, &dmaInfo) < 0) {
        std::cerr << "Failed to get DMA engine information: " << strerror(errno) << std::endl;
        return false;
    }
    
    if (dmaInfo.count == 0) {
        std::cerr << "No DMA engines found" << std::endl;
        return false;
    }
    
    // Initialize DMA engines
    m_dmaEngines.resize(dmaInfo.count);
    
    for (uint32_t i = 0; i < dmaInfo.count; i++) {
        m_dmaEngines[i].baseAddress = dmaInfo.engines[i].baseAddress;
        m_dmaEngines[i].channelCount = dmaInfo.engines[i].channelCount;
        m_dmaEngines[i].busy.store(false);
        
        // Reset the engine
        volatile uint32_t* controlReg = reinterpret_cast<volatile uint32_t*>(
            m_dmaEngines[i].baseAddress + DMA_REG_CONTROL);
        *controlReg = DMA_CTRL_RESET;
        
        // Wait for reset to complete
        volatile uint32_t* statusReg = reinterpret_cast<volatile uint32_t*>(
            m_dmaEngines[i].baseAddress + DMA_REG_STATUS);
        
        int timeout = 1000;  // 1000 iterations
        while ((*statusReg & DMA_STATUS_BUSY) && timeout-- > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        if (timeout <= 0) {
            std::cerr << "Timeout waiting for DMA engine " << i << " to reset" << std::endl;
            return false;
        }
    }
    
    return true;
}

bool Controller::startDMATransfer(
    std::shared_ptr<Buffer> buffer,
    size_t size,
    TransferDirection direction,
    uint32_t channelId) {
    
    // Get an available DMA engine
    DMAEngine* engine = getAvailableDMAEngine();
    if (!engine) {
        return false;
    }
    
    // Calculate base address for this channel's registers
    uint64_t channelBase = engine->baseAddress + (channelId * 0x100);  // Assume 256-byte stride between channels
    
    // Set up source and destination addresses
    uint64_t hostAddr = buffer->getPhysicalAddress();
    uint64_t deviceAddr = buffer->getDeviceAddress();
    
    volatile uint32_t* srcAddrLowReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_SRC_ADDR_LOW);
    volatile uint32_t* srcAddrHighReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_SRC_ADDR_HIGH);
    volatile uint32_t* dstAddrLowReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_DST_ADDR_LOW);
    volatile uint32_t* dstAddrHighReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_DST_ADDR_HIGH);
    
    if (direction == TransferDirection::HOST_TO_DEVICE) {
        *srcAddrLowReg = hostAddr & 0xFFFFFFFF;
        *srcAddrHighReg = (hostAddr >> 32) & 0xFFFFFFFF;
        *dstAddrLowReg = deviceAddr & 0xFFFFFFFF;
        *dstAddrHighReg = (deviceAddr >> 32) & 0xFFFFFFFF;
    } else {
        *srcAddrLowReg = deviceAddr & 0xFFFFFFFF;
        *srcAddrHighReg = (deviceAddr >> 32) & 0xFFFFFFFF;
        *dstAddrLowReg = hostAddr & 0xFFFFFFFF;
        *dstAddrHighReg = (hostAddr >> 32) & 0xFFFFFFFF;
    }
    
    // Set transfer size
    volatile uint32_t* sizeReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_TRANSFER_SIZE);
    *sizeReg = size;
    
    // Set a unique transfer ID
    static uint32_t nextTransferId = 1;
    volatile uint32_t* idReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_TRANSFER_ID);
    *idReg = nextTransferId++;
    
    // Start the transfer with the appropriate direction
    volatile uint32_t* controlReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_CONTROL);
    uint32_t controlValue = DMA_CTRL_START | DMA_CTRL_IRQ_EN;
    
    if (direction == TransferDirection::DEVICE_TO_HOST) {
        controlValue |= DMA_CTRL_DIR_D2H;
    } else {
        controlValue |= DMA_CTRL_DIR_H2D;
    }
    
    *controlReg = controlValue;
    
    return true;
}

bool Controller::checkDMATransferComplete(uint32_t channelId, uint32_t& bytesTransferred) {
    // For simplicity, check all engines
    for (const auto& engine : m_dmaEngines) {
        // Calculate base address for this channel's registers
        uint64_t channelBase = engine.baseAddress + (channelId * 0x100);  // Assume 256-byte stride
        
        // Check status register
        volatile uint32_t* statusReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_STATUS);
        uint32_t status = *statusReg;
        
        if (status & DMA_STATUS_ERROR) {
            // Transfer encountered an error
            bytesTransferred = 0;
            return true;  // Signal completion even though it failed
        }
        
        if (status & DMA_STATUS_DONE) {
            // Transfer completed successfully
            volatile uint32_t* bytesReg = reinterpret_cast<volatile uint32_t*>(
                channelBase + DMA_REG_TRANSFERRED_BYTES);
            bytesTransferred = *bytesReg;
            return true;
        }
    }
    
    // Not complete yet
    return false;
}

bool Controller::abortDMATransfer(uint32_t channelId) {
    for (const auto& engine : m_dmaEngines) {
        // Calculate base address for this channel's registers
        uint64_t channelBase = engine.baseAddress + (channelId * 0x100);  // Assume 256-byte stride
        
        // Issue abort command
        volatile uint32_t* controlReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_CONTROL);
        *controlReg = DMA_CTRL_ABORT;
        
        // Wait for abort to complete
        volatile uint32_t* statusReg = reinterpret_cast<volatile uint32_t*>(channelBase + DMA_REG_STATUS);
        
        int timeout = 1000;  // 1000 iterations
        while ((*statusReg & DMA_STATUS_BUSY) && timeout-- > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        if (timeout <= 0) {
            std::cerr << "Timeout waiting for DMA transfer to abort" << std::endl;
            return false;
        }
    }
    
    return true;
}

Controller::DMAEngine* Controller::getAvailableDMAEngine() {
    for (auto& engine : m_dmaEngines) {
        bool expected = false;
        if (engine.busy.compare_exchange_strong(expected, true)) {
            return &engine;
        }
    }
    
    return nullptr;
}

} // namespace DMA
