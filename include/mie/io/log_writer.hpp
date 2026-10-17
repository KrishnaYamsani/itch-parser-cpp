#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

namespace mie {

// Owns the log file and an io_uring instance. write_batch() submits one write
// for a whole buffer of log lines.
class LogWriter {
public:
    explicit LogWriter(const char* path, unsigned queue_depth = 32) {
        // O_TRUNC: each run starts with a fresh file, so line counts are comparable.
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (fd_ < 0) { std::perror("open log file"); return; }

        int rc = io_uring_queue_init(queue_depth, &ring_, 0);
        if (rc < 0) {
            std::fprintf(stderr, "io_uring_queue_init: %s\n", std::strerror(-rc));
            ::close(fd_);
            fd_ = -1;
            return;
        }
        ring_ready_ = true;
    }

    ~LogWriter() {
        if (ring_ready_) io_uring_queue_exit(&ring_);
        if (fd_ >= 0) ::close(fd_);
    }

    // ring_ holds kernel-shared state, so copying would be wrong.
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    bool ok() const { return fd_ >= 0 && ring_ready_; }

    // Writes the whole buffer, waits for completion, then clears it.
    void write_batch(std::string& buffer) {
        if (buffer.empty()) return;

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) { std::fprintf(stderr, "io_uring: submission queue full\n"); return; }

        io_uring_prep_write(sqe, fd_, buffer.data(), buffer.size(), 0);
        io_uring_submit(&ring_);

        io_uring_cqe* cqe;
        int rc = io_uring_wait_cqe(&ring_, &cqe);
        if (rc < 0) {
            std::fprintf(stderr, "io_uring_wait_cqe: %s\n", std::strerror(-rc));
            return;
        }
        if (cqe->res < 0)
            std::fprintf(stderr, "write failed: %s\n", std::strerror(-cqe->res));
        else if (static_cast<size_t>(cqe->res) != buffer.size())
            std::fprintf(stderr, "short write: %d of %zu bytes\n", cqe->res, buffer.size());
        io_uring_cqe_seen(&ring_, cqe);

        buffer.clear();
    }

private:
    io_uring ring_{};
    int fd_ = -1;
    bool ring_ready_ = false;
};

} // namespace mie