#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include "mie/parser/byte_utils.hpp"

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

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(9999);
    dest.sin_addr.s_addr = inet_addr("127.0.0.1");

    while (offset + 2 <= buf.size()) {
        uint16_t msg_len = mie::read_be16(&buf[offset]);
        if (offset + 2 + msg_len > buf.size()) break;  // truncated tail, same guard as the parser

        ssize_t sent = sendto(sock, &buf[offset], msg_len + 2, 0,
                               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent < 0) { std::perror("sendto"); break; }

        offset += 2 + msg_len;
        sent_count++;
    }

    std::printf("Sent %zu messages\n", sent_count);

    // sentinel: a zero-length "message" signals end-of-stream to the receiver
    uint16_t zero_len = 0;
    uint8_t sentinel[2];
    sentinel[0] = 0;
    sentinel[1] = 0;
    sendto(sock, sentinel, sizeof(sentinel), 0,
        reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        
    close(sock);
    return 0;
}