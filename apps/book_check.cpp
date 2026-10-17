#include <cstdio>
#include <fstream>
#include <vector>
#include "mie/engine/order_book.hpp"
#include "mie/engine/packet_parser.hpp"
#include "mie/parser/byte_utils.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <itch_file>\n", argv[0]); return 1; }

    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    if (!f) { std::perror("open"); return 1; }
    std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), buf.size());

    mie::MarketState market;
    mie::ParsedMessage msg;
    size_t offset = 0, n = 0;
    while (offset + 2 <= buf.size()) {
        const uint16_t len = mie::read_be16(&buf[offset]);
        if (offset + 2 + len > buf.size()) break;
        uint8_t type = 0;
        if (mie::parse_packet(&buf[offset], 2 + len, msg, type)) market.apply(msg);
        offset += 2 + len;
        ++n;
    }
    std::printf("messages: %zu\n", n);
    market.print_report("file replay (no network)", true);
    return 0;
}