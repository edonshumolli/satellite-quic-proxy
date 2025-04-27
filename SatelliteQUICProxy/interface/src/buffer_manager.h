/**
 * @file buffer_manager.h
 * @brief Manages DMA buffers for transfers between host and FPGA
 * 
 * This class provides functionality to allocate, manage, and release
 * DMA-capable memory buffers that can be used for high-speed data transfers
 * between the host system and the FPGA.
 */

#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>

namespace DMA {

/**
 * @class Buffer
 * @brief Represents a DMA-capable memory buffer
 */
class Buffer {
public:
    /**
     * Constructor
     * 
     * @param virtualAddress Virtual address of the buffer in host memory
     * @param physicalAddress Physical address of the buffer in host memory
     * @param deviceAddress Address of the buffer as seen from the FPGA
     * @param size Size of the buffer in bytes
     * @param id Unique identifier for this buffer
     */
    Buffer(void* virtualAddress, uint64_t physicalAddress, uint64_t deviceAddress, 
          size_t size, uint32_t id);
    
    /**
     * Destructor - does not free memory, that's handled by BufferManager
     */
    ~Buffer();
    
    /**
     * Get the virtual address of the buffer
     * 
     * @return Pointer to the buffer in the host's virtual address space
     */
    void* getVirtualAddress() const;
    
    /**
     * Get the physical address of the buffer
     * 
     * @return Physical address of the buffer in the host's memory
     */
    uint64_t getPhysicalAddress() const;
    
    /**
     * Get the device address of the buffer
     * 
     * @return Address of the buffer as seen from the FPGA
     */
    uint64_t getDeviceAddress() const;
    
    /**
     * Get the size of the buffer
     * 
     * @return Size in bytes
     */
    size_t getSize() const;
    
    /**
     * Get the unique ID of this buffer
     * 
     * @return Buffer ID
     */
    uint32_t getID() const;

private:
    void* m_virtualAddress;
    uint64_t m_physicalAddress;
    uint64_t m_deviceAddress;
    size_t m_size;
    uint32_t m_id;
};

/**
 * @class BufferManager
 * @brief Manages allocation and deallocation of DMA buffers
 */
class BufferManager {
public:
    /**
     * Constructor
     * 
     * @param deviceFd File descriptor for the FPGA device
     */
    BufferManager(int deviceFd);
    
    /**
     * Destructor ensures all allocated buffers are freed
     */
    ~BufferManager();
    
    /**
     * Initialize the buffer manager
     * 
     * @return true if initialization successful
     */
    bool initialize();
    
    /**
     * Allocate a DMA buffer of the specified size
     * 
     * @param size Size of the buffer in bytes
     * @return Shared pointer to the allocated buffer or nullptr on failure
     */
    std::shared_ptr<Buffer> allocateBuffer(size_t size);
    
    /**
     * Free a previously allocated buffer
     * 
     * @param buffer Buffer to free
     * @return true if freed successfully
     */
    bool freeBuffer(std::shared_ptr<Buffer> buffer);
    
    /**
     * Get the number of currently allocated buffers
     * 
     * @return Buffer count
     */
    size_t getAllocatedBufferCount() const;
    
    /**
     * Get the total amount of memory currently allocated
     * 
     * @return Total bytes allocated
     */
    size_t getTotalAllocatedMemory() const;

private:
    // Device file descriptor
    int m_deviceFd;
    
    // Flag indicating if initialization succeeded
    bool m_initialized;
    
    // Counter for generating unique buffer IDs
    uint32_t m_nextBufferId;
    
    // Mutex for thread safety
    mutable std::mutex m_buffersMutex;
    
    // Container for tracking allocated buffers
    struct BufferInfo {
        void* virtualAddress;
        uint64_t physicalAddress;
        uint64_t deviceAddress;
        size_t size;
        uint32_t id;
        bool allocated;
        
        BufferInfo() : virtualAddress(nullptr), physicalAddress(0), deviceAddress(0),
                      size(0), id(0), allocated(false) {}
    };
    
    std::vector<BufferInfo> m_buffers;
    
    // Total allocated memory
    size_t m_totalAllocatedMemory;
    
    // Helper methods
    bool allocatePhysicalBuffer(size_t size, BufferInfo& bufferInfo);
    bool freePhysicalBuffer(BufferInfo& bufferInfo);
};

} // namespace DMA

#endif // BUFFER_MANAGER_H
