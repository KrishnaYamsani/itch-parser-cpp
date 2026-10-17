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
#include <liburing.h>
#include <fcntl.h>

#include "mie/parser/byte_utils.hpp"
#include "mie/parser/messages.hpp"
#include "mie/core/pool_allocator.hpp"
#include "mie/core/spsc_ring.hpp"
#include "mie/engine/parsed_message.hpp"

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

void flush_log(struct io_uring* ring_uring, int fd, std::string& buffer){
    if(buffer.empty()) return;

    struct io_uring_sqe* sqe=io_uring_get_sqe(ring_uring);

    io_uring_prep_write(sqe,fd, buffer.data(),buffer.size(),0);
    io_uring_submit(ring_uring);

    struct io_uring_cqe* cqe;
    io_uring_wait_cqe(ring_uring, &cqe);

    if(cqe->res < 0){
        std::fprintf(stderr, "write failed: %s\n", strerror(-cqe->res));
    }

    io_uring_cqe_seen(ring_uring,cqe);
    buffer.clear();
}

int main(int argc,char** argv){
    
    std::array<uint64_t,256> counts{};
    size_t total=0;

    

    auto pool = std::make_unique<mie::PoolAllocator<ParsedMessage, 65536>>();
    auto ring = std::make_unique<mie::SpscRing<ParsedMessage*, 65536>>();
    std::atomic<bool> producer_done{false};

    auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&](){

        int sock = socket(AF_INET,SOCK_DGRAM,0);
        int recvbuf_size = 8*1024*1024;

        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_port=htons(9999);
        addr.sin_addr.s_addr=INADDR_ANY;

        if(setsockopt(sock,SOL_SOCKET,SO_RCVBUF,&recvbuf_size,sizeof(recvbuf_size)) < 0){
            perror("setsockopt SO_RCVBUF");
        }

        struct timeval so_timeout;
        so_timeout.tv_sec = 10;
        so_timeout.tv_usec = 0;

        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &so_timeout, sizeof(so_timeout)) < 0) {
            perror("setsockopt SO_RCVTIMEO");
        }

        if(bind(sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){
            perror("bind");
            producer_done.store(true, std::memory_order_release);
            return;
        }
        
        size_t recvpackets=0;

        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        uint8_t buffers[BATCH_SIZE][MAX_PACKET_SIZE];

        for(int i=0;i<BATCH_SIZE;i++){
            iovecs[i].iov_base = buffers[i];
            iovecs[i].iov_len = MAX_PACKET_SIZE;
            msgs[i].msg_hdr.msg_iov = &iovecs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = nullptr;
            msgs[i].msg_hdr.msg_namelen = 0;
        }

        // struct timespec timeout;
        // timeout.tv_sec = 2;     // 2 seconds of silence = assume stream ended
        // timeout.tv_nsec = 0;

        Duration time_recv{0};
        Duration time_parse{0};
        Duration time_alloc{0};
        Duration time_push{0};

        while(true){
            auto t_recv_start = Clock::now();
            int received = recvmmsg(sock,msgs,BATCH_SIZE,0,nullptr);
            time_recv += Clock::now() - t_recv_start;

            if(received<0){
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::printf("Timed out waiting for more packets. Total received: %zu\n", recvpackets);
                    break;
                }

                perror("recvmmsg");
                break;
            }
            bool endstream=false;

            for(int i=0;i<received;i++){
                uint8_t* packet=buffers[i];
                size_t packet_len = msgs[i].msg_len;

                if(packet_len==2 && packet[0]==0 && packet[1]==0){
                    endstream=true;
                    break;
                }

                if(packet_len < 3) continue;

                recvpackets++;
                uint16_t msg_len=mie::read_be16(&packet[0]);
                uint8_t msg_type=packet[2];
                
                if(2 + msg_len > packet_len) continue;
                
                auto t_alloc_start = Clock::now();
                auto* p=pool->allocate();
                while(!p){ p=pool->allocate(); }
                p->kind = Msgkind::Other;
                time_alloc += Clock::now() - t_alloc_start;

                counts[msg_type]++;
            
                const uint8_t* body=&packet[2];

                auto t_parse_start = Clock::now();
                if(msg_type == 'A'){
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
                time_parse += Clock::now() - t_parse_start;

                auto t_push_start = Clock::now();
                while(!ring->push(p)){}
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

        close(sock);
    });

    int log_fd = open("engine_log.txt",O_WRONLY | O_CREAT | O_APPEND,0644);
    if(log_fd < 0){ perror("open log file"); return 1;}
    
    struct io_uring ring_uring;
    if(io_uring_queue_init(32,&ring_uring,0) < 0){
        perror("io_uring_queue_init");
        return 1;
    }


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
                flush_log(&ring_uring,log_fd, log_buffer);
                since_flush=0;
            }

            pool->deallocate(msg);
            consumed++;
        }

        flush_log(&ring_uring, log_fd, log_buffer); 
        std::printf("Consumer processed %zu messages\n", consumed);
    });

    producer.join();
    consumer.join();

    io_uring_queue_exit(&ring_uring);
    close(log_fd);

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