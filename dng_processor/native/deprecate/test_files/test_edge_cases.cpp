#include "DngDecoder.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdio>

#define CLR_GREEN "\033[32m"
#define CLR_RED "\033[31m"
#define CLR_RESET "\033[0m"
#define PASS CLR_GREEN "[PASS]" CLR_RESET
#define FAIL CLR_RED "[FAIL]" CLR_RESET

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << FAIL << " " << msg << "\n";                                 \
      g_failed++;                                                              \
    } else {                                                                   \
      std::cout << PASS << " " << msg << "\n";                                 \
      g_passed++;                                                              \
    }                                                                          \
  } while (0)

int main() {
    std::cout << "--- Test Edge Cases: Invalid DNG ---\n";
    DngDecoder decoder;
    
    {
        DngMetadata metadata = {};
        DngErrorCode code = decoder.decodeFile("/path/to/non_existent_file.dng", metadata);
        ASSERT_TRUE(code != DngErrorCode::SUCCESS, "Decoder correctly rejects non-existent file");
    }

    {
        const char* junkPath = "junk_test.dng";
        std::ofstream ofs(junkPath, std::ios::binary);
        std::vector<char> junk(1024);
        for(int i=0; i<1024; ++i) junk[i] = (char)(i % 256);
        ofs.write(junk.data(), junk.size());
        ofs.close();

        DngMetadata metadata = {};
        DngErrorCode code = decoder.decodeFile(junkPath, metadata);
        ASSERT_TRUE(code != DngErrorCode::SUCCESS, "Decoder correctly rejects junk file");
        
        std::remove(junkPath);
    }
    
    return g_failed > 0 ? 1 : 0;
}
