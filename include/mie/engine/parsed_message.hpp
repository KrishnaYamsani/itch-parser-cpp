#pragma once
#include <cstdint>
#include "mie/parser/messages.hpp"

namespace mie {

enum class Msgkind : uint8_t {
    Other,              // message types we don't decode (S, R, H, N, ...)
    Add,
    Delete,
    Executed,
    ExecutedWithPrice,
    Replace,
    Cancel
};

// One fixed-size, self-describing event. `kind` says which union member is valid.
struct ParsedMessage {
    Msgkind kind;
    union {
        AddOrderMessage add;
        DeleteOrderMessage del;
        OrderExecutedMessage exec;
        OrderExecutedWithPriceMessage exec_price;
        OrderReplaceMessage replace;
        CancelOrderMessage cancel;
    };
};

} // namespace mie