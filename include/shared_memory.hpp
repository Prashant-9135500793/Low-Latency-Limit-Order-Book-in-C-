#pragma once
// shared_memory.hpp
//
// RAII wrapper around a file-backed mmap() shared-memory mapping.
//
// We use a plain file-backed mapping (open()/ftruncate()/mmap() on a
// regular file) rather than POSIX shm_open() purely for clarity and
// portability of the demo: the resulting file
// (/tmp/lob_shared_memory.dat by default) is easy to inspect,
// `ls -l` shows its size, and cleanup is `rm` on a real path. The
// underlying mechanism (MAP_SHARED over a file-backed region visible
// to multiple processes) is the same idea POSIX shared memory uses,
// which itself is typically backed by tmpfs.
//
// IMPORTANT CORRECTNESS NOTE: mmap() does NOT bypass the kernel. It
// asks the kernel to establish a shared virtual-memory mapping; the
// OS still manages page tables, may take page faults on first touch,
// and the CPU still performs virtual->physical address translation
// (TLB) on every access. What mmap() buys us is that *after* the
// mapping is established, reads and writes to that memory happen
// directly from user space via ordinary loads/stores -- there is no
// per-message syscall, no copy into a kernel socket buffer, and no
// serialization step, unlike e.g. writing messages down a pipe or
// socket. That is a real and significant latency win for high message
// rates, but it is not "zero kernel involvement," and it is not a
// guarantee of any particular absolute latency number on its own --
// cache effects, page faults, and scheduler behavior still apply.

#include <cstddef>
#include <string>

namespace lob {

class SharedMemoryRegion {
public:
    SharedMemoryRegion() = default;
    ~SharedMemoryRegion();

    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion(SharedMemoryRegion&& other) noexcept;
    SharedMemoryRegion& operator=(SharedMemoryRegion&& other) noexcept;

    // Creates (or truncates) the backing file, sizes it to `size`
    // bytes, and mmap()s it PROT_READ|PROT_WRITE, MAP_SHARED. Use
    // from the process that initializes the region (typically
    // whichever process starts first / the matching engine).
    // Returns false and leaves the object unmapped on any error.
    bool create(const std::string& path, size_t size);

    // Opens an EXISTING backing file (does not create or resize it)
    // and mmap()s it the same way. Use from processes that attach to
    // an already-initialized region.
    bool open(const std::string& path, size_t size);

    // Unmaps and closes the fd, if mapped. Does NOT delete the file.
    void close();

    void* data() const noexcept { return addr_; }
    size_t size() const noexcept { return size_; }
    bool is_mapped() const noexcept { return addr_ != nullptr; }

    // Removes the backing file from the filesystem. Safe to call even
    // if other processes still have it mapped (POSIX semantics: the
    // mapping stays valid for processes that already mapped it; the
    // path simply stops resolving to it for new opens).
    static bool remove_file(const std::string& path);

private:
    bool map_common(int fd, size_t size, bool is_creator);

    void* addr_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
};

} // namespace lob
