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

    struct AddOrderMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        uint64_t order_ref_num;
        char     buy_sell_indicator;   // 'B' or 'S'
        uint32_t shares;
        std::array<char, 8> stock;
        uint32_t price;                // fixed-point, scaled by 10,000 (e.g. 1234500 = $123.45)
    };

    struct DeleteOrderMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        uint64_t order_ref_num;
    };

    struct OrderExecutedMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        uint64_t order_ref_num;
        uint32_t executed_shares;
        uint64_t match_num;
    };

    struct OrderExecutedWithPriceMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        uint64_t order_ref_num;
        uint32_t executed_shares;
        uint64_t match_num;
        char     printable;
        uint32_t execution_price;
    };

    struct OrderReplaceMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        uint64_t original_order_ref_num;
        uint64_t new_order_ref_num;
        uint32_t shares;
        uint32_t price;
    };

    struct CancelOrderMessage {
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint64_t timestamp_ns;
        uint64_t order_ref_num;
        uint32_t cancelled_shares;
    };
}