/**
 * @file buffer_manager.cpp
 * @brief Implementation of the DMA buffer manager
 */

#include "buffer_manager.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <cstring>
#include <algorithm>

// Device-specific ioctl commands (would be defined in driver header)
#define IOCTL_ALLOC_DMA_BUFFER    0x2004
#define IOCTL_FREE_DMA_BUFFER     0x2005

// Memory alignment requirement for DMA buffers
#define DMA_BUFFER_ALIGNMENT      4096  // 4KB alignment

// Maximum number of buffers that can be allocated
#define MAX_DMA_BUFFERS           64

namespace DMA {

//------------------------------------------------------------------------------
// Buffer implementation
//------------------------------------------------------------------------------

Buffer::Buffer(void* virtualAddress, uint64_t physicalAddress, uint64_t deviceAddress, 
              size_t size, uint32_t id)
    : m_virtualAddress(virtualAddress),
      m_physicalAddress(physicalAddress),
      m_deviceAddress(deviceAddress),
      m_size(size),
      m_id(id) {
}

Buffer::~Buffer() {
    // Buffer destruction is handled by BufferManager
}

void* Buffer::getVirtualAddress() const {
    return m_virtualAddress;
}

uint64_t Buffer::getPhysicalAddress() const {
    return m_physicalAddress;
}

uint64_t Buffer::getDeviceAddress() const {
    return m_deviceAddress;
}

size_t Buffer::getSize() const {
    return m_size;
}

uint32_t Buffer::getID() const {
    return m_id;
}

//------------------------------------------------------------------------------
// BufferManager implementation
//------------------------------------------------------------------------------

BufferManager::BufferManager(int deviceFd)
    : m_deviceFd(deviceFd),
      m_initialized(false),
      m_nextBufferId(1),
      m_totalAllocatedMemory(0) {
}

BufferManager::~BufferManager() {
    // Free all allocated buffers
    std::lock_guard<std::mutex> lock(m_buffersMutex);
    
    for (auto& bufferInfo : m_buffers) {
        if (bufferInfo.allocated) {
            freePhysicalBuffer(bufferInfo);
        }
    }
}

bool BufferManager::initialize() {
    if (m_initialized) {
        return true;  // Already initialized
    }
    
    if (m_deviceFd < 0) {
        std::cerr << "Invalid device file descriptor" << std::endl;
        return false;
    }
    
    // Initialize buffer tracking
    m_buffers.reserve(MAX_DMA_BUFFERS);
    
    m_initialized = true;
    return true;
}

std::shared_ptr<Buffer> BufferManager::allocateBuffer(size_t size) {
    if (!m_initialized) {
        std::cerr << "Buffer manager not initialized" << std::endl;
        return nullptr;
    }
    
    if (size == 0) {
        std::cerr << "Cannot allocate buffer of size 0" << std::endl;
        return nullptr;
    }
    
    // Align size to DMA_BUFFER_ALIGNMENT
    size = (size + DMA_BUFFER_ALIGNMENT - 1) & ~(DMA_BUFFER_ALIGNMENT - 1);
    
    std::lock_guard<std::mutex> lock(m_buffersMutex);
    
    // Check if we have too many buffers already
    if (m_buffers.size() >= MAX_DMA_BUFFERS) {
        std::cerr << "Maximum number of DMA buffers already allocated" << std::endl;
        return nullptr;
    }
    
    // Allocate a new buffer
    BufferInfo bufferInfo;
    if (!allocatePhysicalBuffer(size, bufferInfo)) {
        std::cerr << "Failed to allocate physical buffer" << std::endl;
        return nullptr;
    }
    
    // Assign a unique ID
    bufferInfo.id = m_nextBufferId++;
    bufferInfo.allocated = true;
    
    // Add to tracking container
    m_buffers.push_back(bufferInfo);
    
    // Update total allocated memory
    m_totalAllocatedMemory += size;
    
    // Create and return the Buffer object
    return std::make_shared<Buffer>(
        bufferInfo.virtualAddress,
        bufferInfo.physicalAddress,
        bufferInfo.deviceAddress,
        bufferInfo.size,
        bufferInfo.id
    );
}

bool BufferManager::freeBuffer(std::shared_ptr<Buffer> buffer) {
    if (!m_initialized || !buffer) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_buffersMutex);
    
    // Find the buffer in our tracking container
    auto it = std::find_if(m_buffers.begin(), m_buffers.end(),
        [&buffer](const BufferInfo& info) {
            return info.allocated && info.id == buffer->getID();
        }
    );
    
    if (it == m_buffers.end()) {
        std::cerr << "Buffer not found in manager" << std::endl;
        return false;
    }
    
    // Free the physical buffer
    if (!freePhysicalBuffer(*it)) {
        std::cerr << "Failed to free physical buffer" << std::endl;
        return false;
    }
    
    // Update total allocated memory
    m_totalAllocatedMemory -= it->size;
    
    // Remove from tracking container
    m_buffers.erase(it);
    
    return true;
}

size_t BufferManager::getAllocatedBufferCount() const {
    std::lock_guard<std::mutex> lock(m_buffersMutex);
    return m_buffers.size();
}

size_t BufferManager::getTotalAllocatedMemory() const {
    std::lock_guard<std::mutex> lock(m_buffersMutex);
    return m_totalAllocatedMemory;
}

bool BufferManager::allocatePhysicalBuffer(size_t size, BufferInfo& bufferInfo) {
    // Allocate DMA buffer via ioctl
    struct {
        size_t size;
        void* virtualAddress;
        uint64_t physicalAddress;
        uint64_t deviceAddress;
    } allocParams;
    
    allocParams.size = size;
    allocParams.virtualAddress = nullptr;
    allocParams.physicalAddress = 0;
    allocParams.deviceAddress = 0;
    
    if (ioctl(m_deviceFd, IOCTL_ALLOC_DMA_BUFFER, &allocParams) < 0) {
        std::cerr << "Failed to allocate DMA buffer: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Store buffer information
    bufferInfo.virtualAddress = allocParams.virtualAddress;
    bufferInfo.physicalAddress = allocParams.physicalAddress;
    bufferInfo.deviceAddress = allocParams.deviceAddress;
    bufferInfo.size = size;
    
    return true;
}

bool BufferManager::freePhysicalBuffer(BufferInfo& bufferInfo) {
    if (!bufferInfo.allocated || !bufferInfo.virtualAddress) {
        return false;
    }
    
    // Free DMA buffer via ioctl
    struct {
        void* virtualAddress;
        size_t size;
    } freeParams;
    
    freeParams.virtualAddress = bufferInfo.virtualAddress;
    freeParams.size = bufferInfo.size;
    
    if (ioctl(m_deviceFd, IOCTL_FREE_DMA_BUFFER, &freeParams) < 0) {
        std::cerr << "Failed to free DMA buffer: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Mark as not allocated
    bufferInfo.allocated = false;
    bufferInfo.virtualAddress = nullptr;
    bufferInfo.physicalAddress = 0;
    bufferInfo.deviceAddress = 0;
    
    return true;
}

} // namespace DMA
