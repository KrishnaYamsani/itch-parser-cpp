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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <cerrno>
#include <chrono>

#include "mie/parser/byte_utils.hpp"
#include "mie/parser/messages.hpp"
#include "mie/core/pool_allocator.hpp"
#include "mie/core/spsc_ring.hpp"
#include "mie/engine/parsed_message.hpp"
#include "mie/io/udp_receiver.hpp"
#include "mie/io/log_writer.hpp"
#include "mie/engine/packet_parser.hpp"

using mie::Msgkind;
using mie::ParsedMessage;

#define BATCH_SIZE 64
#define MAX_PACKET_SIZE 2048

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;

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

std::string format_log_line(ParsedMessage* msg){
    char line[128];
    int len=0;

    switch (msg->kind) {
        case Msgkind::Add:
            len = std::snprintf(line, sizeof(line), "ADD ref=%llu price=%u\n",
                                 (unsigned long long)msg->add.order_ref_num, msg->add.price);
            break;
        case Msgkind::Delete:
            len = std::snprintf(line, sizeof(line), "DEL ref=%llu\n",
                                 (unsigned long long)msg->del.order_ref_num);
            break;
        case Msgkind::Executed:
            len = std::snprintf(line, sizeof(line), "EXEC ref=%llu shares=%u\n",
                                 (unsigned long long)msg->exec.order_ref_num, msg->exec.executed_shares);
            break;
        default:
            len = std::snprintf(line, sizeof(line), "OTHER\n");
            break;
    }
    return std::string(line, len);
}


int main(int argc,char** argv){
    
    std::array<uint64_t,256> counts{};
    size_t total=0;

    auto pool = std::make_unique<mie::PoolAllocator<ParsedMessage, 65536>>();
    auto ring = std::make_unique<mie::SpscRing<ParsedMessage*, 65536>>();
    mie::LogWriter log_writer("engine_log.txt");
    if (!log_writer.ok()) return 1;
    std::atomic<bool> producer_done{false};

    auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&](){

        mie::UdpReceiver receiver(9999, 8 * 1024 * 1024, 10);
        if (!receiver.ok()) {
            producer_done.store(true, std::memory_order_release);
            return;
        }
        size_t recvpackets = 0;

        // struct timespec timeout;
        // timeout.tv_sec = 2;     // 2 seconds of silence = assume stream ended
        // timeout.tv_nsec = 0;

        Duration time_recv{0};
        Duration time_parse{0};
        Duration time_alloc{0};
        Duration time_push{0};

        while(true){
            auto t_recv_start = Clock::now();
            int received = receiver.receive_batch();
            time_recv += Clock::now() - t_recv_start;

            if (received < 0) {
                if (receiver.timed_out()) {
                    std::printf("Timed out waiting for more packets. Total received: %zu\n", recvpackets);
                } else {
                    perror("recvmmsg");
                }
                break;
            }
            bool endstream=false;

            for(int i=0;i<received;i++){
                const uint8_t* packet = receiver.packet(i);
                size_t packet_len = receiver.packet_len(i);

                if(packet_len==2 && packet[0]==0 && packet[1]==0){
                    endstream=true;
                    break;
                }

                recvpackets++;

                auto t_alloc_start = Clock::now();
                auto* p = pool->allocate();
                while (!p) { p = pool->allocate(); }
                time_alloc += Clock::now() - t_alloc_start;

                auto t_parse_start = Clock::now();
                uint8_t msg_type = 0;
                const bool parsed = mie::parse_packet(packet, packet_len, *p, msg_type);
                time_parse += Clock::now() - t_parse_start;

                if (!parsed) {          // malformed: give the slot back and skip it
                    pool->deallocate(p);
                    continue;
                }

                counts[msg_type]++;

                auto t_push_start = Clock::now();
                while (!ring->push(p)) {}
                time_push += Clock::now() - t_push_start;

                total++;
            }
            
            if (endstream) {
                std::printf("Received end-of-stream sentinel. Total received: %zu\n", recvpackets);
                break;  // exit the loop cleanly
            }
        }

        std::printf("time_recv:  %.3f s\n", time_recv.count());
        std::printf("time_alloc: %.3f s\n", time_alloc.count());
        std::printf("time_parse: %.3f s\n", time_parse.count());
        std::printf("time_push:  %.3f s\n", time_push.count());

        producer_done.store(true,std::memory_order_release);

    });

    std::thread consumer([&](){

        size_t consumed = 0;
        std::string log_buffer;
        constexpr size_t FLUSH_THRESHOLD = 1000;
        size_t since_flush=0;

        while (true) {
            auto v = ring->pop();
            ParsedMessage* msg = nullptr;

            if (v) {
                msg = *v;
            } else if (producer_done.load(std::memory_order_acquire)) {
                auto final_check = ring->pop();  // one last try — catches anything
                                                // pushed in the gap above
                if (final_check) {
                    msg=*final_check;
                }else{
                    break;  // genuinely done
                }
            }else{
                continue;
            }

            log_buffer += format_log_line(msg);
            since_flush++;

            if(since_flush == FLUSH_THRESHOLD){
                log_writer.write_batch(log_buffer);
                since_flush=0;
            }

            pool->deallocate(msg);
            consumed++;
        }

        log_writer.write_batch(log_buffer);
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