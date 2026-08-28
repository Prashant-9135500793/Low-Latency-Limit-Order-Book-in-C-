#include "shared_memory.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lob {

namespace {
void log_errno(const char* what) {
    std::fprintf(stderr, "[shared_memory] %s failed: %s\n", what, std::strerror(errno));
}
} // namespace

SharedMemoryRegion::~SharedMemoryRegion() { close(); }

SharedMemoryRegion::SharedMemoryRegion(SharedMemoryRegion&& other) noexcept
    : addr_(other.addr_), size_(other.size_), fd_(other.fd_) {
    other.addr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

SharedMemoryRegion& SharedMemoryRegion::operator=(SharedMemoryRegion&& other) noexcept {
    if (this != &other) {
        close();
        addr_ = other.addr_;
        size_ = other.size_;
        fd_ = other.fd_;
        other.addr_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

bool SharedMemoryRegion::map_common(int fd, size_t size, bool /*is_creator*/) {
    void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        log_errno("mmap");
        ::close(fd);
        return false;
    }
    fd_ = fd;
    addr_ = addr;
    size_ = size;
    return true;
}

bool SharedMemoryRegion::create(const std::string& path, size_t size) {
    close();
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        log_errno("open(O_CREAT)");
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        log_errno("ftruncate");
        ::close(fd);
        return false;
    }
    return map_common(fd, size, /*is_creator=*/true);
}

bool SharedMemoryRegion::open(const std::string& path, size_t size) {
    close();
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        log_errno("open");
        return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        log_errno("fstat");
        ::close(fd);
        return false;
    }
    if (static_cast<size_t>(st.st_size) < size) {
        std::fprintf(stderr,
                      "[shared_memory] backing file smaller than expected "
                      "(%zu < %zu); is the creator process running?\n",
                      static_cast<size_t>(st.st_size), size);
        ::close(fd);
        return false;
    }
    return map_common(fd, size, /*is_creator=*/false);
}

void SharedMemoryRegion::close() {
    if (addr_ != nullptr) {
        ::munmap(addr_, size_);
        addr_ = nullptr;
        size_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SharedMemoryRegion::remove_file(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        log_errno("unlink");
        return false;
    }
    return true;
}

} // namespace lob
