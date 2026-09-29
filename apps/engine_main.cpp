#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>

#include "mie/parser/byte_utils.hpp"
#include "mie/parser/messages.hpp"

std::vector<uint8_t> read_whole_file(const char* path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f){std::perror("open");std::exit(1);}

    auto size = f.tellg();
    std::vector<uint8_t> buf(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()),size);//using char* as ifstream read takes char*
    return buf;
}

const char* msg_name(uint8_t t) {
    switch (t) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'h': return "Operational Halt";
        case 'A': return "Add Order (no MPID)";
        case 'F': return "Add Order (with MPID)";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed With Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'N': return "Retail Price Improvement";
        default:  return "(unknown)";
    }
}

int main(int argc,char** argv){
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch_file>\n", argv[0]);
        return 1;
    }

    auto buf=read_whole_file(argv[1]);
    std::array<uint64_t,256> counts{};
    size_t offset=0;//sizze_t because offset is used for indexing and also to store high enough index values
    size_t total=0;

    auto t0=std::chrono::steady_clock::now();

    while(offset+2 <= buf.size()){
        uint16_t msg_len = mie::read_be16(&buf[offset]);
        offset += 2;
        if(offset + msg_len > buf.size()) break;

        char msg_type=buf[offset];
        counts[msg_type]++;

        offset += msg_len;
        total++;
    }

    auto t1=std::chrono::steady_clock::now();
    double secs=std::chrono::duration<double>(t1 - t0).count();

    std::printf("%-6s %-30s %12s %8s\n", "type", "name", "count", "share");
    for (int t = 0; t < 256; ++t) {
        if (counts[t] == 0) continue;
        std::printf("%-6c %-30s %12llu %7.2f%%\n",
                    t, msg_name(static_cast<uint8_t>(t)),
                    (unsigned long long)counts[t],
                    100.0 * counts[t] / total);
    }

    std::printf("\nTotal: %zu messages in %.3f s  (%.1f M msgs/s)\n",
                total, secs, total / secs / 1e6);
    return 0;
}