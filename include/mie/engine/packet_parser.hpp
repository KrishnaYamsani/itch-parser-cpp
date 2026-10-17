#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "mie/engine/parsed_message.hpp"
#include "mie/parser/byte_utils.hpp"
#include "mie/parser/messages.hpp"

namespace mie {

namespace detail {
// Every order-book message starts with the same three fields after the type byte.
template <typename Msg>
inline void read_common_header(Msg& m, const uint8_t* body) {
    m.stock_locate    = read_be16(body + 1);
    m.tracking_number = read_be16(body + 3);
    m.timestamp_ns    = read_be48(body + 5);
}
} // namespace detail

// Decodes one framed packet: [2-byte length][body, starting with the type byte].
// Returns false if the packet is malformed (truncated, or too short for its type).
// For types we don't decode (S, R, H, ...) it returns true with out.kind == Other.
inline bool parse_packet(const uint8_t* packet, size_t packet_len,
                         ParsedMessage& out, uint8_t& msg_type) {
    if (packet_len < 3) return false;

    const uint16_t msg_len = read_be16(packet);
    if (msg_len < 1 || 2u + msg_len > packet_len) return false;

    msg_type = packet[2];
    const uint8_t* body = packet + 2;
    out.kind = Msgkind::Other;

    switch (msg_type) {
    case 'A': {
        if (msg_len < 36) return false;
        AddOrderMessage m;
        detail::read_common_header(m, body);
        m.order_ref_num      = read_be64(body + 11);
        m.buy_sell_indicator = static_cast<char>(body[19]);
        m.shares             = read_be32(body + 20);
        std::memcpy(m.stock.data(), body + 24, 8);
        m.price              = read_be32(body + 32);
        out.kind = Msgkind::Add;
        out.add = m;
        return true;
    }
    case 'D': {
        if (msg_len < 19) return false;
        DeleteOrderMessage m;
        detail::read_common_header(m, body);
        m.order_ref_num = read_be64(body + 11);
        out.kind = Msgkind::Delete;
        out.del = m;
        return true;
    }
    case 'E': {
        if (msg_len < 31) return false;
        OrderExecutedMessage m;
        detail::read_common_header(m, body);
        m.order_ref_num   = read_be64(body + 11);
        m.executed_shares = read_be32(body + 19);
        m.match_num       = read_be64(body + 23);
        out.kind = Msgkind::Executed;
        out.exec = m;
        return true;
    }
    case 'C': {
        if (msg_len < 36) return false;
        OrderExecutedWithPriceMessage m;
        detail::read_common_header(m, body);
        m.order_ref_num   = read_be64(body + 11);
        m.executed_shares = read_be32(body + 19);
        m.match_num       = read_be64(body + 23);
        m.printable       = static_cast<char>(body[31]);
        m.execution_price = read_be32(body + 32);
        out.kind = Msgkind::ExecutedWithPrice;
        out.exec_price = m;
        return true;
    }
    case 'U': {
        if (msg_len < 35) return false;
        OrderReplaceMessage m;
        detail::read_common_header(m, body);
        m.original_order_ref_num = read_be64(body + 11);
        m.new_order_ref_num      = read_be64(body + 19);
        m.shares                 = read_be32(body + 27);
        m.price                  = read_be32(body + 31);
        out.kind = Msgkind::Replace;
        out.replace = m;
        return true;
    }
    case 'X': {
        if (msg_len < 23) return false;
        CancelOrderMessage m;
        detail::read_common_header(m, body);
        m.order_ref_num    = read_be64(body + 11);
        m.cancelled_shares = read_be32(body + 19);
        out.kind = Msgkind::Cancel;
        out.cancel = m;
        return true;
    }
    default:
        return true;   // valid frame, type we don't decode
    }
}

} // namespace mie