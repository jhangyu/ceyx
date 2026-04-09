#include <iostream>
#include <stdlib.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>

#include "HalideBuffer.h"
#include "HalideRuntimeMetal.h"

extern "C" id MTLCreateSystemDefaultDevice();

int main() {
    id device = MTLCreateSystemDefaultDevice();
    
    int width = 1000;
    int height = 1000;
    size_t size = width * height * 4;
    
    void* host_ptr = nullptr;
    posix_memalign(&host_ptr, 4096, size);
    
    SEL newBufSel = sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:");
    id mtl_buffer = ((id (*)(id, SEL, void*, size_t, size_t, id))objc_msgSend)(
        device, newBufSel, host_ptr, size, 0, nil
    );
    
    if (mtl_buffer) {
        // We must CFRetain it if wrap_buffer expects to own it?
        // Wait, newBufferWith... returns an object with +1 retain count.
        // Halide wrap_buffer: "does not take ownership of the buffer". Wait, docs say:
        // "Frees any storage associated with the binding... but does not free the MTLBuffer."
        // Let's check Halide docs.
    }
    
    {
        Halide::Runtime::Buffer<uint8_t> halide_out = Halide::Runtime::Buffer<uint8_t>::make_interleaved(
            static_cast<uint8_t*>(host_ptr), width, height, 4);
            
        int err = halide_metal_wrap_buffer(nullptr, halide_out.raw_buffer(), (uint64_t)mtl_buffer);
        std::cout << "Wrap err: " << err << "\n";
        
        err = halide_out.device_sync();
        std::cout << "Sync err: " << err << "\n";
    } // halide_out goes out of scope here.
    
    if (mtl_buffer) {
        CFRelease((CFTypeRef)mtl_buffer);
    }
    free(host_ptr);
    std::cout << "Success\n";
    return 0;
}
