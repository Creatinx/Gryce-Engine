#include "script/big_number.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gryce_engine::script {

namespace {

constexpr int k_base = 10;

// 比较两个非负数字串（little-endian）：>0 表示 a>b，==0 相等，<0 表示 a<b
int compare_magnitude(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

std::vector<uint8_t> add_magnitude(const std::vector<uint8_t>& a,
                                   const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out;
    out.reserve(std::max(a.size(), b.size()) + 1);
    int carry = 0;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int d = carry + (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        out.push_back(static_cast<uint8_t>(d % k_base));
        carry = d / k_base;
    }
    if (carry) out.push_back(static_cast<uint8_t>(carry));
    return out;
}

// 前提：|a| >= |b|
std::vector<uint8_t> sub_magnitude(const std::vector<uint8_t>& a,
                                   const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out;
    out.reserve(a.size());
    int borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int d = a[i] - borrow - (i < b.size() ? b[i] : 0);
        if (d < 0) {
            d += k_base;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out.push_back(static_cast<uint8_t>(d));
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

std::vector<uint8_t> mul_magnitude(const std::vector<uint8_t>& a,
                                   const std::vector<uint8_t>& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<uint8_t> out(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        int carry = 0;
        for (size_t j = 0; j < b.size() || carry; ++j) {
            const int cur = out[i + j] + carry + (j < b.size() ? a[i] * b[j] : 0);
            out[i + j] = static_cast<uint8_t>(cur % k_base);
            carry = cur / k_base;
        }
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// BigInt
// ---------------------------------------------------------------------------
BigInt::BigInt(int64_t v) {
    if (v < 0) {
        negative_ = true;
        v = -v;
    }
    if (v == 0) return;
    while (v > 0) {
        digits_.push_back(static_cast<uint8_t>(v % k_base));
        v /= k_base;
    }
}

BigInt BigInt::from_string(const std::string& s, std::string* err) {
    BigInt out;
    size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        out.negative_ = (s[i] == '-');
        ++i;
    }
    bool any = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            if (err) *err = "invalid big integer: " + s;
            return BigInt();
        }
        if (c == '0' && !any) continue;  // 跳过前导零
        out.digits_.push_back(static_cast<uint8_t>(c - '0'));
        any = true;
    }
    if (!any) {
        out.negative_ = false;
    }
    std::reverse(out.digits_.begin(), out.digits_.end());  // 转为 little-endian
    out.trim();
    return out;
}

std::string BigInt::to_string() const {
    if (digits_.empty()) return "0";
    std::string s;
    if (negative_) s.push_back('-');
    for (size_t i = digits_.size(); i-- > 0;) {
        s.push_back(static_cast<char>('0' + digits_[i]));
    }
    return s;
}

void BigInt::trim() {
    while (!digits_.empty() && digits_.back() == 0) digits_.pop_back();
    if (digits_.empty()) negative_ = false;
}

int BigInt::compare(const BigInt& o) const {
    if (negative_ != o.negative_) return negative_ ? -1 : 1;
    if (is_zero() && o.is_zero()) return 0;
    const int cmp = compare_magnitude(digits_, o.digits_);
    return negative_ ? -cmp : cmp;
}

BigInt BigInt::abs() const {
    BigInt out = *this;
    out.negative_ = false;
    return out;
}

BigInt BigInt::neg() const {
    BigInt out = *this;
    if (!out.is_zero()) out.negative_ = !out.negative_;
    return out;
}

BigInt BigInt::add(const BigInt& o) const {
    if (negative_ == o.negative_) {
        BigInt out;
        out.digits_ = add_magnitude(digits_, o.digits_);
        out.negative_ = negative_ && !out.is_zero();
        return out;
    }
    // 异号：转为绝对值相减
    const int cmp = compare_magnitude(digits_, o.digits_);
    BigInt out;
    if (cmp == 0) return out;
    if (cmp > 0) {
        out.digits_ = sub_magnitude(digits_, o.digits_);
        out.negative_ = negative_;
    } else {
        out.digits_ = sub_magnitude(o.digits_, digits_);
        out.negative_ = o.negative_;
    }
    return out;
}

BigInt BigInt::sub(const BigInt& o) const {
    return add(o.neg());
}

BigInt BigInt::mul(const BigInt& o) const {
    BigInt out;
    out.digits_ = mul_magnitude(digits_, o.digits_);
    out.negative_ = negative_ != o.negative_ && !out.is_zero();
    return out;
}

BigInt BigInt::divmod(const BigInt& o, BigInt& rem) const {
    rem = BigInt();
    if (o.is_zero()) return BigInt();

    // 用绝对值做长除法
    const std::vector<uint8_t> divisor = o.digits_;
    std::vector<uint8_t> r;
    std::vector<uint8_t> q;
    q.reserve(digits_.size());
    for (size_t i = digits_.size(); i-- > 0;) {
        // r = r * 10 + digit
        r = mul_magnitude(r, {10});
        if (digits_[i] != 0) r = add_magnitude(r, {digits_[i]});
        int d = 0;
        while (compare_magnitude(r, divisor) >= 0) {
            r = sub_magnitude(r, divisor);
            ++d;
        }
        q.push_back(static_cast<uint8_t>(d));
    }
    std::reverse(q.begin(), q.end());
    while (!q.empty() && q.back() == 0) q.pop_back();

    // 商与余数的符号
    BigInt quot;
    quot.digits_ = q;
    quot.negative_ = (negative_ != o.negative_) && !quot.is_zero();
    rem.digits_ = r;
    rem.negative_ = negative_ && !rem.is_zero();
    return quot;
}

BigInt BigInt::div(const BigInt& o) const {
    BigInt rem;
    return divmod(o, rem);
}

BigInt BigInt::mod(const BigInt& o) const {
    BigInt rem;
    divmod(o, rem);
    return rem;
}

BigInt BigInt::pow(int exp) const {
    if (exp < 0) return BigInt();
    BigInt base = *this;
    BigInt result(1);
    while (exp > 0) {
        if (exp & 1) result = result.mul(base);
        exp >>= 1;
        if (exp) base = base.mul(base);
    }
    return result;
}

BigInt BigInt::pow10(int n) {
    BigInt out;
    // little-endian：10^n = n 个 0 后跟一个 1
    out.digits_.resize(static_cast<size_t>(n), 0);
    out.digits_.push_back(1);
    return out;
}

// ---------------------------------------------------------------------------
// BigDecimal
// ---------------------------------------------------------------------------
BigDecimal::BigDecimal(int64_t v, int scale) : coeff_(v), scale_(scale) {}

BigDecimal BigDecimal::from_string(const std::string& s, std::string* err) {
    if (s.empty()) {
        if (err) *err = "empty big decimal";
        return BigDecimal();
    }

    size_t i = 0;
    bool neg = false;
    if (s[i] == '-' || s[i] == '+') {
        neg = (s[i] == '-');
        ++i;
    }

    std::string digits;
    int scale = 0;
    bool saw_dot = false;
    bool any = false;
    for (; i < s.size() && s[i] != 'e' && s[i] != 'E'; ++i) {
        const char c = s[i];
        if (c == '.') {
            if (saw_dot) {
                if (err) *err = "invalid big decimal: " + s;
                return BigDecimal();
            }
            saw_dot = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            if (err) *err = "invalid big decimal: " + s;
            return BigDecimal();
        }
        digits.push_back(c);
        if (saw_dot) ++scale;
        any = true;
    }

    int exp = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            eneg = (s[i] == '-');
            ++i;
        }
        for (; i < s.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
                if (err) *err = "invalid big decimal exponent: " + s;
                return BigDecimal();
            }
            exp = exp * 10 + (s[i] - '0');
        }
        if (eneg) exp = -exp;
    }

    scale -= exp;

    BigDecimal out;
    if (!any) {
        out.coeff_ = BigInt();
        out.scale_ = 0;
        return out;
    }

    std::string coeff_str = neg ? "-" + digits : digits;
    out.coeff_ = BigInt::from_string(coeff_str, err);
    if (out.coeff_.is_zero()) {
        out.scale_ = 0;
        return out;
    }
    out.scale_ = scale;
    if (out.scale_ < 0) {
        out.coeff_ = out.coeff_.mul(BigInt::pow10(-out.scale_));
        out.scale_ = 0;
    }
    return out;
}

BigDecimal BigDecimal::from_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return from_string(buf);
}

std::string BigDecimal::to_string() const {
    if (coeff_.is_zero()) return "0";
    // 规范化：去掉系数末尾的 0（tostring 输出最简形式，如 0.3 而非 0.300...0）
    int scale = scale_;
    BigInt c = coeff_;
    const BigInt ten(10);
    while (scale > 0 && !c.is_zero() && c.mod(ten).is_zero()) {
        c = c.div(ten);
        --scale;
    }
    const bool neg = c.is_negative();
    std::string s = c.abs().to_string();
    if (scale <= 0) {
        if (scale < 0) {
            s.append(static_cast<size_t>(-scale), '0');
        }
    } else {
        const size_t point = static_cast<size_t>(scale);
        if (s.size() <= point) {
            s.insert(s.begin(), point + 1 - s.size(), '0');
        }
        s.insert(s.end() - point, '.');
    }
    if (neg) s.insert(s.begin(), '-');
    return s;
}

double BigDecimal::to_double() const {
    return std::strtod(to_string().c_str(), nullptr);
}

int BigDecimal::compare(const BigDecimal& o) const {
    const int common = std::max(scale_, o.scale_);
    BigInt a = coeff_;
    BigInt b = o.coeff_;
    if (scale_ < common) a = a.mul(BigInt::pow10(common - scale_));
    if (o.scale_ < common) b = b.mul(BigInt::pow10(common - o.scale_));
    return a.compare(b);
}

BigDecimal BigDecimal::abs() const {
    BigDecimal out = *this;
    out.coeff_ = out.coeff_.abs();
    return out;
}

BigDecimal BigDecimal::neg() const {
    BigDecimal out = *this;
    out.coeff_ = out.coeff_.neg();
    return out;
}

BigDecimal BigDecimal::add(const BigDecimal& o) const {
    const int common = std::max(scale_, o.scale_);
    BigInt a = coeff_;
    BigInt b = o.coeff_;
    if (scale_ < common) a = a.mul(BigInt::pow10(common - scale_));
    if (o.scale_ < common) b = b.mul(BigInt::pow10(common - o.scale_));
    BigDecimal out;
    out.coeff_ = a.add(b);
    out.scale_ = common;
    if (out.coeff_.is_zero()) out.scale_ = 0;
    return out;
}

BigDecimal BigDecimal::sub(const BigDecimal& o) const {
    return add(o.neg());
}

BigDecimal BigDecimal::mul(const BigDecimal& o) const {
    BigDecimal out;
    out.coeff_ = coeff_.mul(o.coeff_);
    out.scale_ = scale_ + o.scale_;
    if (out.coeff_.is_zero()) out.scale_ = 0;
    return out;
}

BigDecimal BigDecimal::div(const BigDecimal& o, int precision) const {
    if (o.is_zero()) return BigDecimal();
    if (precision < 0) precision = 0;

    const int target = precision + o.scale_;
    BigInt a = coeff_;
    if (target > scale_) {
        a = a.mul(BigInt::pow10(target - scale_));
    } else if (target < scale_) {
        const BigInt factor = BigInt::pow10(scale_ - target);
        BigInt rem;
        const BigInt q = a.divmod(factor, rem);
        a = q;
        if (!rem.is_zero() && rem.abs().mul(BigInt(2)).compare(factor.abs()) >= 0) {
            a = a.add(BigInt(a.sign() == 0 ? coeff_.sign() : a.sign()));
        }
    }

    BigInt rem;
    BigInt q = a.divmod(o.coeff_, rem);
    if (!rem.is_zero() && rem.abs().mul(BigInt(2)).compare(o.coeff_.abs()) >= 0) {
        if (q.is_zero()) {
            q = BigInt(a.sign() * o.coeff_.sign());
        } else {
            q = q.add(BigInt(q.sign()));
        }
    }
    BigDecimal out;
    out.coeff_ = q;
    out.scale_ = precision;
    if (out.coeff_.is_zero()) out.scale_ = 0;
    return out;
}

BigDecimal BigDecimal::pow(int exp, int precision) const {
    if (precision < 0) precision = 0;
    if (exp == 0) return BigDecimal(1, 0);
    if (exp < 0) {
        BigDecimal inv = BigDecimal(1, 0).div(*this, precision + 4);
        return inv.pow(-exp, precision);
    }
    BigDecimal base = set_precision(precision + 4);
    BigDecimal result(1, 0);
    while (exp > 0) {
        if (exp & 1) result = result.mul(base).set_precision(precision + 4);
        exp >>= 1;
        if (exp) base = base.mul(base).set_precision(precision + 4);
    }
    return result.set_precision(precision);
}

BigDecimal BigDecimal::sqrt(int precision) const {
    if (is_zero()) return BigDecimal();
    if (sign() < 0) return BigDecimal();
    if (precision < 0) precision = 0;

    const int work = precision + 8;
    BigDecimal x = from_double(std::sqrt(to_double()));
    if (x.is_zero()) x = BigDecimal(1, 0);
    for (int i = 0; i < 64; ++i) {
        BigDecimal next = x.add(div(x, work + 4)).mul(BigDecimal(5, 1)).set_precision(work);
        if (next.compare(x) == 0) break;
        x = next;
    }
    return x.set_precision(precision);
}

BigDecimal BigDecimal::floor() const {
    if (scale_ <= 0) return *this;
    const BigInt factor = BigInt::pow10(scale_);
    BigInt rem;
    BigInt q = coeff_.divmod(factor, rem);
    if (coeff_.is_negative() && !rem.is_zero()) q = q.sub(BigInt(1));
    BigDecimal out;
    out.coeff_ = q;
    out.scale_ = 0;
    return out;
}

BigDecimal BigDecimal::ceil() const {
    if (scale_ <= 0) return *this;
    const BigInt factor = BigInt::pow10(scale_);
    BigInt rem;
    BigInt q = coeff_.divmod(factor, rem);
    if (!coeff_.is_negative() && !rem.is_zero()) q = q.add(BigInt(1));
    BigDecimal out;
    out.coeff_ = q;
    out.scale_ = 0;
    return out;
}

BigDecimal BigDecimal::round() const {
    if (scale_ <= 0) return *this;
    const BigInt factor = BigInt::pow10(scale_);
    BigInt rem;
    BigInt q = coeff_.divmod(factor, rem);
    if (!rem.is_zero() && rem.abs().mul(BigInt(2)).compare(factor) >= 0) {
        q = q.add(BigInt(q.is_zero() ? coeff_.sign() : q.sign()));
    }
    BigDecimal out;
    out.coeff_ = q;
    out.scale_ = 0;
    return out;
}

BigDecimal BigDecimal::set_precision(int n) const {
    if (n < 0) n = 0;
    if (n >= scale_) {
        BigDecimal out;
        out.coeff_ = coeff_.mul(BigInt::pow10(n - scale_));
        out.scale_ = n;
        return out;
    }
    const BigInt factor = BigInt::pow10(scale_ - n);
    BigInt rem;
    BigInt q = coeff_.divmod(factor, rem);
    if (!rem.is_zero() && rem.abs().mul(BigInt(2)).compare(factor) >= 0) {
        q = q.add(BigInt(q.is_zero() ? coeff_.sign() : q.sign()));
    }
    BigDecimal out;
    out.coeff_ = q;
    out.scale_ = n;
    if (out.coeff_.is_zero()) out.scale_ = 0;
    return out;
}

} // namespace gryce_engine::script
