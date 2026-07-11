#include "uuid.h"

#include <random>
#include <sstream>
#include <iomanip>

namespace gryce_engine::scene {

namespace {

std::string generate_uuid_string() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    // UUID v4 格式：xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx，其中 y = 8,9,a,b
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            ss << '-';
        } else if (i == 14) {
            ss << '4';
        } else if (i == 19) {
            ss << std::hex << (8 + (dis(gen) & 0x03));
        } else {
            ss << dis(gen);
        }
    }
    return ss.str();
}

} // namespace

UUID::UUID() : value_(generate_uuid_string()) {}

UUID::UUID(const std::string& str) : value_(str) {}

bool UUID::is_valid() const {
    if (value_.size() != 36) return false;
    for (int i = 0; i < 36; ++i) {
        char c = value_[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
    }
    return true;
}

UUID UUID::nil() {
    return UUID("00000000-0000-0000-0000-000000000000");
}

UUID UUID::generate() {
    return UUID();
}

} // namespace gryce_engine::scene
