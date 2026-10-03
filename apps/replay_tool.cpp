#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>

#include "mie/parser/byte_utils.hpp"

#define SEND_BATCH_SIZE 64

std::vector<uint8_t> read_whole_file(const char* path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f){std::perror("open");std::exit(1);}
    auto size = f.tellg();
    std::vector<uint8_t> buf(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()),size);
    return buf;
}

int main(int argc,char** argv){
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch_file>\n", argv[0]);
        return 1;
    }

    auto buf = read_whole_file(argv[1]);
    size_t offset = 0;
    size_t sent_count = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { std::perror("socket"); return 1; }

    int sndbuf_size = 8 * 1024 * 1024;  // 8MB, matching what we used for SO_RCVBUF
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) < 0) {
        perror("setsockopt SO_SNDBUF");
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(9999);
    dest.sin_addr.s_addr = inet_addr("127.0.0.1");

    struct mmsghdr send_msgs[SEND_BATCH_SIZE] = {};
    struct iovec send_iovecs[SEND_BATCH_SIZE] = {};

    for (int i = 0; i < SEND_BATCH_SIZE; ++i) {
        send_msgs[i].msg_hdr.msg_iov = &send_iovecs[i];
        send_msgs[i].msg_hdr.msg_iovlen = 1;
        send_msgs[i].msg_hdr.msg_name = &dest;
        send_msgs[i].msg_hdr.msg_namelen = sizeof(dest);
    }

    auto t0 = std::chrono::steady_clock::now();

    while (offset + 2 <= buf.size()) {
        int batch_count = 0;

        while(batch_count < SEND_BATCH_SIZE && offset + 2 <= buf.size()){
            uint16_t msg_len = mie::read_be16(&buf[offset]);
            if (offset + 2 + msg_len > buf.size()) break;

            send_iovecs[batch_count].iov_base = &buf[offset];
            send_iovecs[batch_count].iov_len  = 2 + msg_len;

            offset += 2 + msg_len;
            sent_count++;
            batch_count++;
        }
        
        if (batch_count == 0) break;

        int sent_now = sendmmsg(sock, send_msgs, batch_count, 0);
        if (sent_now < 0) { perror("sendmmsg"); break; }
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Sent %zu messages in %.3f s (%.1f M msgs/s)\n", sent_count, secs, sent_count/secs/1e6);

    // sentinel: a zero-length "message" signals end-of-stream to the receiver
    uint16_t zero_len = 0;
    uint8_t sentinel[2];
    sentinel[0] = 0;
    sentinel[1] = 0;
    for (int i = 0; i < 5; ++i) {
        sendto(sock, sentinel, sizeof(sentinel), 0,
            reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }

    close(sock);
    return 0;
}