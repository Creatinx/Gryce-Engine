#pragma once

#include <string>

namespace gryce_engine::scene {

// ---------------------------------------------------------------------------
// UUID — 轻量字符串 UUID
// 格式：xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
// 先用字符串实现，后续可优化为 128-bit 整数。
// ---------------------------------------------------------------------------
class UUID {
public:
    UUID();
    explicit UUID(const std::string& str);

    const std::string& str() const { return value_; }
    bool is_valid() const;

    bool operator==(const UUID& other) const { return value_ == other.value_; }
    bool operator!=(const UUID& other) const { return value_ != other.value_; }
    bool operator<(const UUID& other) const { return value_ < other.value_; }

    static UUID nil();
    static UUID generate();

private:
    std::string value_;
};

} // namespace gryce_engine::scene

namespace std {
template<>
struct hash<gryce_engine::scene::UUID> {
    std::size_t operator()(const gryce_engine::scene::UUID& id) const {
        return std::hash<std::string>()(id.str());
    }
};
} // namespace std
