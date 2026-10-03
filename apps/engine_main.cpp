#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <thread>
#include <atomic>

#include "mie/parser/byte_utils.hpp"
#include "mie/parser/messages.hpp"
#include "mie/core/pool_allocator.hpp"
#include "mie/core/spsc_ring.hpp"


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

    enum class Msgkind:uint8_t {Add,Delete,Executed,ExecutedWithPrice,Replace,Cancel};

    struct ParsedMessage{
        Msgkind kind;
        union{
            mie::AddOrderMessage add;
            mie::DeleteOrderMessage del;
            mie::OrderExecutedMessage exec;
            mie::OrderExecutedWithPriceMessage exec_price;
            mie::OrderReplaceMessage replace;
            mie::CancelOrderMessage cancel;
        };
    };

    mie::SpscRing<ParsedMessage*,1024> ring;
    mie::PoolAllocator<ParsedMessage,1024> pool;
    std::atomic<bool> producer_done{false};

    auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&](){

        while(offset+2 <= buf.size()){
            uint16_t msg_len = mie::read_be16(&buf[offset]);
            offset += 2;
            if(offset + msg_len > buf.size()) break;

            auto* p=pool.allocate();
            while(!p){
                p=pool.allocate();
            }

            uint8_t msg_type=buf[offset];
            counts[msg_type]++;
            
            
            const uint8_t* body=&buf[offset];

            if(msg_type == 'A'){
                if (counts['A'] == 1) {
                    std::printf("DEBUG: first 'A' message body starts at file offset %zu\n", offset);
                }
                mie::AddOrderMessage m;
                m.stock_locate = mie::read_be16(body+1);
                m.tracking_number = mie::read_be16(body+3);
                m.timestamp_ns = mie::read_be48(body+5);
                m.order_ref_num = mie::read_be64(body+11);
                m.buy_sell_indicator = (char)body[19];
                m.shares = mie::read_be32(body+20);
                std::memcpy(m.stock.data(),body+24,8);
                m.price = mie::read_be32(body+32);
                
                p->kind = Msgkind::Add;
                p->add = m;
            }else if(msg_type == 'D'){
                mie::DeleteOrderMessage m;
                m.stock_locate = mie::read_be16(body+1);
                m.tracking_number = mie::read_be16(body+3);
                m.timestamp_ns = mie::read_be48(body+5);
                m.order_ref_num = mie::read_be64(body+11);

                p->kind = Msgkind::Delete;
                p->del=m;
            }else if (msg_type == 'E') {
                mie::OrderExecutedMessage m;
                m.stock_locate    = mie::read_be16(body + 1);
                m.tracking_number = mie::read_be16(body + 3);
                m.timestamp_ns    = mie::read_be48(body + 5);
                m.order_ref_num   = mie::read_be64(body + 11);
                m.executed_shares = mie::read_be32(body + 19);
                m.match_num       = mie::read_be64(body + 23);

                p->exec = m;
                p->kind = Msgkind::Executed;
            }
            else if (msg_type == 'C') {
                mie::OrderExecutedWithPriceMessage m;
                m.stock_locate      = mie::read_be16(body + 1);
                m.tracking_number   = mie::read_be16(body + 3);
                m.timestamp_ns      = mie::read_be48(body + 5);
                m.order_ref_num     = mie::read_be64(body + 11);
                m.executed_shares   = mie::read_be32(body + 19);
                m.match_num         = mie::read_be64(body + 23);
                m.printable          = static_cast<char>(body[31]);
                m.execution_price   = mie::read_be32(body + 32);

                p->exec_price = m;
                p->kind=Msgkind::ExecutedWithPrice;
            }
            else if (msg_type == 'U') {
                mie::OrderReplaceMessage m;
                m.stock_locate            = mie::read_be16(body + 1);
                m.tracking_number         = mie::read_be16(body + 3);
                m.timestamp_ns            = mie::read_be48(body + 5);
                m.original_order_ref_num  = mie::read_be64(body + 11);
                m.new_order_ref_num       = mie::read_be64(body + 19);
                m.shares                  = mie::read_be32(body + 27);
                m.price                   = mie::read_be32(body + 31);

                p->replace = m;
                p->kind = Msgkind::Replace;
            }
            else if (msg_type == 'X') {
                mie::CancelOrderMessage m;
                m.stock_locate      = mie::read_be16(body + 1);
                m.tracking_number    = mie::read_be16(body + 3);
                m.timestamp_ns       = mie::read_be48(body + 5);
                m.order_ref_num      = mie::read_be64(body + 11);
                m.cancelled_shares   = mie::read_be32(body + 19);

                p->cancel = m;
                p->kind = Msgkind::Cancel;
            }

            while(!ring.push(p)){}

            offset += msg_len;
            total++;
            //if(total>=5000) break;
        }
        producer_done.store(true,std::memory_order_release);
    });

    std::thread consumer([&](){

        size_t consumed = 0;
        while (true) {
            auto v = ring.pop();
            if (v) {
                ParsedMessage* msg = *v;
                pool.deallocate(msg);
                consumed++;
                continue;
            }
            if (producer_done.load(std::memory_order_acquire)) {
                auto final_check = ring.pop();  // one last try — catches anything
                                                // pushed in the gap above
                if (final_check) {
                    pool.deallocate(*final_check);
                    consumed++;
                    continue;
                }
                break;  // genuinely done
            }
        }
        std::printf("Consumer processed %zu messages\n", consumed);
    });

    producer.join();
    consumer.join();

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

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