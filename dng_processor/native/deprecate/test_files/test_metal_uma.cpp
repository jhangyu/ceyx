#include <iostream>
#include <chrono>
#include <objc/message.h>
#include "HalideBuffer.h"
#include "HalideRuntimeMetal.h"

int main() {
    int width = 4000;
    int height = 3000;
    
    Halide::Runtime::Buffer<uint8_t> buf(width, height, 4);
    
    auto t0 = std::chrono::steady_clock::now();
    int err = buf.device_malloc(halide_metal_device_interface());
    auto t1 = std::chrono::steady_clock::now();
    
    std::cout << "device_malloc err: " << err << ", " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";
    
    uintptr_t mtl_buf = halide_metal_get_buffer(nullptr, buf.raw_buffer());
    std::cout << "MTLBuffer: " << std::hex << mtl_buf << std::dec << "\n";
    
    if (mtl_buf) {
        id mtl_buf_id = (id)mtl_buf;
        void* contents = ((void* (*)(id, SEL))objc_msgSend)(mtl_buf_id, sel_registerName("contents"));
        std::cout << "Contents pointer: " << contents << "\n";
        
        uint64_t storageMode = ((uint64_t (*)(id, SEL))objc_msgSend)(mtl_buf_id, sel_registerName("storageMode"));
        std::cout << "Storage Mode (0=Shared, 1=Managed, 2=Private, 3=Memoryless): " << storageMode << "\n";
    }
    
    return 0;
}
