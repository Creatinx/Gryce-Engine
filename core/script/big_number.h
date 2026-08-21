#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::script {

// ---------------------------------------------------------------------------
// BigInt — 任意精度有符号整数（十进制 digit 数组，little-endian）
// 用于 GryceSRT 的 big 模块：超出 double 精度的大整数精确计算。
// ---------------------------------------------------------------------------
class BigInt {
public:
    BigInt() = default;                       // 0
    explicit BigInt(int64_t v);

    static BigInt from_string(const std::string& s, std::string* err = nullptr);
    std::string to_string() const;

    bool is_zero() const { return digits_.empty(); }
    bool is_negative() const { return negative_; }
    int sign() const { return is_zero() ? 0 : (negative_ ? -1 : 1); }

    int compare(const BigInt& o) const;
    BigInt abs() const;
    BigInt neg() const;

    BigInt add(const BigInt& o) const;
    BigInt sub(const BigInt& o) const;
    BigInt mul(const BigInt& o) const;
    // 除法：返回商；余数写入 rem（除零时 rem 置 0、返回 0）
    BigInt divmod(const BigInt& o, BigInt& rem) const;
    BigInt div(const BigInt& o) const;
    BigInt mod(const BigInt& o) const;
    BigInt pow(int exp) const;

    // 供 BigDecimal 使用
    static BigInt pow10(int n);
    int digit_count() const { return static_cast<int>(digits_.size()); }

private:
    void trim();

    // 低位在前：digits_[0] 是个位
    std::vector<uint8_t> digits_;
    bool negative_ = false;
};

// ---------------------------------------------------------------------------
// BigDecimal — 超高精度十进制浮点：BigInt 系数 + 小数位（scale）
// value = coeff * 10^(-scale)。用于高精度/超高精度数值计算。
// ---------------------------------------------------------------------------
class BigDecimal {
public:
    BigDecimal() = default;                   // 0
    explicit BigDecimal(int64_t v, int scale = 0);

    static BigDecimal from_string(const std::string& s, std::string* err = nullptr);
    static BigDecimal from_double(double v);
    std::string to_string() const;
    double to_double() const;

    bool is_zero() const { return coeff_.is_zero(); }
    int sign() const { return coeff_.sign(); }
    int scale() const { return scale_; }
    bool is_integer() const { return scale_ <= 0; }

    int compare(const BigDecimal& o) const;
    BigDecimal abs() const;
    BigDecimal neg() const;

    BigDecimal add(const BigDecimal& o) const;
    BigDecimal sub(const BigDecimal& o) const;
    BigDecimal mul(const BigDecimal& o) const;
    // 除法：结果保留 precision 位小数
    BigDecimal div(const BigDecimal& o, int precision) const;
    BigDecimal pow(int exp, int precision) const;
    BigDecimal sqrt(int precision) const;

    BigDecimal floor() const;
    BigDecimal ceil() const;
    BigDecimal round() const;
    // 四舍五入到 n 位小数（n<0 表示舍入到整数左侧）
    BigDecimal set_precision(int n) const;

private:
    BigInt coeff_;
    int scale_ = 0;
};

} // namespace gryce_engine::script
