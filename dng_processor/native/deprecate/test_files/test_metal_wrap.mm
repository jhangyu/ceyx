#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <iostream>
#include <chrono>
#include <stdlib.h>
#include "HalideBuffer.h"
#include "HalideRuntimeMetal.h"

int main() {
    int width = 4000;
    int height = 3000;
    size_t size = width * height * 4;
    
    // Page align allocation
    void* host_ptr = nullptr;
    posix_memalign(&host_ptr, 4096, size);
    uint8_t* out = static_cast<uint8_t*>(host_ptr);
    
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLBuffer> mtl_buffer = [device newBufferWithBytesNoCopy:out
                                       length:size
                                       options:MTLResourceStorageModeShared
                                       deallocator:nil];
                                       
    Halide::Runtime::Buffer<uint8_t> buf = Halide::Runtime::Buffer<uint8_t>::make_interleaved(out, width, height, 4);
    
    // Wrap it
    int err = halide_metal_wrap_buffer(nullptr, buf.raw_buffer(), (uint64_t)mtl_buffer);
    
    std::cout << "Wrap err: " << err << "\n";
    std::cout << "Is device mapped: " << buf.has_device_allocation() << "\n";
    
    // Try to get buffer back
    uintptr_t ret_buf = halide_metal_get_buffer(nullptr, buf.raw_buffer());
    std::cout << "ret_buf: " << std::hex << ret_buf << " expected: " << mtl_buffer << std::dec << "\n";
    
    free(out);
    return 0;
}
