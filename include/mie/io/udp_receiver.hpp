#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mie {

// Batched UDP receiver built on recvmmsg. Owns the socket and all batch buffers.
class UdpReceiver {
public:
    static constexpr int kBatchSize = 64;
    static constexpr size_t kMaxPacketSize = 2048;

    UdpReceiver(uint16_t port, int rcvbuf_bytes, int timeout_sec)
        : buffers_(kBatchSize * kMaxPacketSize),
          iovecs_(kBatchSize),
          msgs_(kBatchSize) {          // vector value-initializes: every field zeroed
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { std::perror("socket"); return; }

        // Big kernel receive buffer: absorbs bursts instead of silently dropping.
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_bytes, sizeof(rcvbuf_bytes)) < 0)
            std::perror("setsockopt SO_RCVBUF");
        
        int actual = 0;
        socklen_t alen = sizeof(actual);
        getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &actual, &alen);
        std::printf("SO_RCVBUF requested %d, kernel granted %d\n", rcvbuf_bytes, actual);
        
        // Socket-level timeout. recvmmsg's own timeout argument is buggy
        // (see the BUGS section of the man page), so we don't use it.
        timeval tv{};
        tv.tv_sec = timeout_sec;
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            std::perror("setsockopt SO_RCVTIMEO");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("bind");
            ::close(fd_);
            fd_ = -1;
            return;
        }

        // One-time wiring: slot i receives into its own region of buffers_.
        for (int i = 0; i < kBatchSize; ++i) {
            iovecs_[i].iov_base = &buffers_[i * kMaxPacketSize];
            iovecs_[i].iov_len = kMaxPacketSize;
            msgs_[i].msg_hdr.msg_iov = &iovecs_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
        }
    }

    ~UdpReceiver() { if (fd_ >= 0) ::close(fd_); }

    // msgs_ holds pointers into iovecs_/buffers_, so copying or moving would leave them dangling.
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool ok() const { return fd_ >= 0; }

    // Blocks until at least one packet arrives. Returns the packet count,
    // or -1 on error/timeout (check timed_out() to tell which).
    int receive_batch() {
        timed_out_ = false;
        int n = ::recvmmsg(fd_, msgs_.data(), kBatchSize, 0, nullptr);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) timed_out_ = true;
        return n;
    }

    bool timed_out() const { return timed_out_; }

    const uint8_t* packet(int i) const { return &buffers_[i * kMaxPacketSize]; }
    size_t packet_len(int i) const { return msgs_[i].msg_len; }

private:
    std::vector<uint8_t> buffers_;
    std::vector<iovec> iovecs_;
    std::vector<mmsghdr> msgs_;
    int fd_ = -1;
    bool timed_out_ = false;
};

} // namespace mie