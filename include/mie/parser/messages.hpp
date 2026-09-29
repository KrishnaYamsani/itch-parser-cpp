#pragma once
#include <cstdint>
#include <array>

namespace mie{
    struct SystemEventMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        char event_code;
    };

    struct StockDirectoryMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        std::array<char,8> stock;
        char market_category;
        char financial_status_indicator;
        uint32_t round_lot_size;
        char round_lots_only;
    };
}