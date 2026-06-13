#include "parser.hpp"

MessageParser::MessageParser() {
    std::memset(padded_buf_ + kBufferCapacity, 0, simdjson::SIMDJSON_PADDING);
}
