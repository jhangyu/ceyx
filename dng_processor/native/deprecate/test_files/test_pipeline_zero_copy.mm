#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <iostream>
#include <chrono>
#include <stdlib.h>
#include "HalideBuffer.h"
#include "HalideRuntimeMetal.h"
#include "dng_pipeline.h"

int main() {
    int width = 4000;
    int height = 3000;
    size_t size = width * height * 4;
    
    // Page align allocation
    void* host_ptr = nullptr;
    posix_memalign(&host_ptr, 4096, size);
    uint8_t* out = static_cast<uint8_t*>(host_ptr);
    
    // Fill dummy alpha
    for(size_t i=0; i<size; i+=4) { out[i+3] = 255; }
    
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLBuffer> mtl_buffer = [device newBufferWithBytesNoCopy:out
                                       length:size
                                       options:MTLResourceStorageModeShared
                                       deallocator:nil];
                                       
    Halide::Runtime::Buffer<uint8_t> buf = Halide::Runtime::Buffer<uint8_t>::make_interleaved(out, width, height, 3);
    buf.raw_buffer()->dim[0].stride = 4;
    buf.raw_buffer()->dim[1].stride = width * 4;
    buf.raw_buffer()->dim[2].stride = 1;
    
    int err = halide_metal_wrap_buffer(nullptr, buf.raw_buffer(), (uint64_t)mtl_buffer);
    
    std::cout << "Wrap err: " << err << "\n";
    
    // Test the pipeline
    // Wait, the pipeline requires many inputs. I can't easily run it.
    // Let's just verify if halide_metal_wrap_buffer prevents copy_to_host.
    
    buf.set_device_dirty(); // Simulate that GPU modified it
    
    auto t0 = std::chrono::steady_clock::now();
    buf.copy_to_host();
    auto t1 = std::chrono::steady_clock::now();
    
    std::cout << "copy_to_host: " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";
    
    free(out);
    return 0;
}
