// cleanup_shared_memory.cpp
// Removes the shared-memory backing file so the next run starts
// clean. matching_engine_main also does this automatically on
// startup, but this is handy if you want to reset state without
// starting the engine (e.g. after a crash left the file behind).
//
// Usage: ./cleanup_shared_memory [--path P]

#include <cstdio>
#include <string>

#include "shared_memory.hpp"

int main(int argc, char** argv) {
    std::string path = "/tmp/lob_shared_memory.dat";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--path" && i + 1 < argc) path = argv[++i];
    }
    bool ok = lob::SharedMemoryRegion::remove_file(path);
    std::printf("[cleanup] %s: %s\n", path.c_str(), ok ? "removed (or did not exist)" : "FAILED");
    return ok ? 0 : 1;
}
