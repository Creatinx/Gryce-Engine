#include "pe_image.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace grycegc::pe {

std::vector<std::string> import_dll_names(const std::filesystem::path& path) {
    std::vector<std::string> names;
    std::ifstream f(path, std::ios::binary);
    if (!f) return names;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (data.size() < 64 || data[0] != 'M' || data[1] != 'Z') return names;

    // e_lfanew
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, data.data() + 0x3C, 4);
    if (e_lfanew + 4 > data.size()) return names;
    if (std::memcmp(data.data() + e_lfanew, "PE\0\0", 4) != 0) return names;

    const size_t coff = e_lfanew + 4;  // COFF header 起点
    if (coff + 20 > data.size()) return names;
    uint16_t num_sect = 0;
    std::memcpy(&num_sect, data.data() + coff + 2, 2);
    uint16_t size_opt = 0;
    std::memcpy(&size_opt, data.data() + coff + 16, 2);

    const size_t opt_start = coff + 20;
    if (size_opt < 24 || opt_start + size_opt > data.size()) return names;

    uint16_t magic = 0;
    std::memcpy(&magic, data.data() + opt_start, 2);
    size_t dd_offset, num_rva_off;
    if (magic == 0x20B) {  // PE32+
        dd_offset = opt_start + 112;
        num_rva_off = opt_start + 108;
    } else if (magic == 0x10B) {  // PE32
        dd_offset = opt_start + 96;
        num_rva_off = opt_start + 92;
    } else {
        return names;
    }
    uint32_t num_rva = 0;
    std::memcpy(&num_rva, data.data() + num_rva_off, 4);
    if (num_rva < 2) return names;

    uint32_t import_rva = 0, _ignored = 0;
    std::memcpy(&import_rva, data.data() + dd_offset + 8, 4);  // data dir[1] = import
    std::memcpy(&_ignored, data.data() + dd_offset + 12, 4);
    if (import_rva == 0) return names;

    const size_t sect_start = opt_start + size_opt;

    auto rva_to_off = [&](uint64_t rva) -> std::optional<uint64_t> {
        for (size_t i = 0; i < num_sect; ++i) {
            const size_t sh = sect_start + i * 40;
            if (sh + 40 > data.size()) break;
            uint32_t vsize = 0, vaddr = 0, rawsize = 0, rawptr = 0;
            std::memcpy(&vsize, data.data() + sh + 8, 4);
            std::memcpy(&vaddr, data.data() + sh + 12, 4);
            std::memcpy(&rawsize, data.data() + sh + 16, 4);
            std::memcpy(&rawptr, data.data() + sh + 20, 4);
            const size_t span = std::max<size_t>(vsize, rawsize);
            if (vaddr <= rva && rva < static_cast<uint64_t>(vaddr) + span) {
                return static_cast<uint64_t>(rawptr) + (rva - vaddr);
            }
        }
        return std::nullopt;
    };

    auto off = rva_to_off(import_rva);
    if (!off) return names;
    const size_t base = static_cast<size_t>(*off);

    // 遍历 IMAGE_IMPORT_DESCRIPTOR（每项 20 字节），以 ILT=0 && name=0 终止
    for (size_t i = 0; i < 8192; ++i) {
        const size_t desc = base + i * 20;
        if (desc + 20 > data.size()) break;
        uint32_t ilt = 0, name_rva = 0;
        std::memcpy(&ilt, data.data() + desc, 4);
        std::memcpy(&name_rva, data.data() + desc + 12, 4);
        if (ilt == 0 && name_rva == 0) break;
        auto no = rva_to_off(name_rva);
        if (!no) continue;
        const size_t n = static_cast<size_t>(*no);
        if (n >= data.size()) continue;
        // 找下一个 NUL
        size_t end = n;
        while (end < data.size() && data[end] != 0) ++end;
        std::string nm(reinterpret_cast<const char*>(data.data() + n), end - n);
        if (!nm.empty()) names.push_back(nm);
    }
    return names;
}

PeInfo inspect(const std::filesystem::path& path) {
    PeInfo info;
    std::ifstream f(path, std::ios::binary);
    if (!f) { info.error = "cannot open file"; return info; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (data.size() < 64 || data[0] != 'M' || data[1] != 'Z') {
        info.error = "not a PE file";
        return info;
    }
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, data.data() + 0x3C, 4);
    if (e_lfanew + 4 > data.size()) { info.error = "bad e_lfanew"; return info; }
    if (std::memcmp(data.data() + e_lfanew, "PE\0\0", 4) != 0) {
        info.error = "missing PE signature";
        return info;
    }
    const size_t coff = e_lfanew + 4;
    if (coff + 20 > data.size()) { info.error = "short COFF"; return info; }
    std::memcpy(&info.machine, data.data() + coff, 2);
    const size_t opt_start = coff + 20;
    uint16_t size_opt = 0;
    std::memcpy(&size_opt, data.data() + coff + 16, 2);
    if (size_opt < 2 || opt_start + 2 > data.size()) { info.error = "short opt"; return info; }
    uint16_t magic = 0;
    std::memcpy(&magic, data.data() + opt_start, 2);
    info.is_pe32_plus = (magic == 0x20B);
    info.ok = true;
    return info;
}

namespace {

// 找文件末尾的 GryceGC trailer（若有）。
// trailer 布局：[magic(8)][plen(4)][payload(plen)]，magic 位于 size-12-plen。
// 从文件尾向前扫描 magic，取最后一个满足长度自洽（m+12+plen == size）的位置。
bool find_trailer(const std::vector<uint8_t>& data, size_t& offset, size_t& total_size) {
    if (data.size() < kTrailerHeaderLen) return false;
    for (size_t m = data.size() - kTrailerHeaderLen + 1; m-- > 0;) {
        if (std::memcmp(data.data() + m, kTrailerMagic, kTrailerMagicLen) != 0) continue;
        uint32_t plen = 0;
        std::memcpy(&plen, data.data() + m + kTrailerMagicLen, 4);
        const size_t total = kTrailerHeaderLen + plen;
        if (m + total != data.size()) continue;  // 长度不自洽，继续向前找
        offset = m;
        total_size = total;
        return true;
    }
    return false;
}

} // namespace

bool read_trailer(const std::filesystem::path& path, Trailer& out) {
    out.payload.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    size_t off = 0, total = 0;
    if (!find_trailer(data, off, total)) return true;
    out.payload.assign(reinterpret_cast<const char*>(data.data() + off + kTrailerHeaderLen),
                       data.size() - off - kTrailerHeaderLen);
    return true;
}

std::vector<uint8_t> strip_trailer(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    size_t off = 0, total = 0;
    if (!find_trailer(data, off, total)) return data;
    data.resize(off);
    return data;
}

bool write_trailer(const std::filesystem::path& path, const Trailer& in) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    // 去掉已有 trailer
    size_t off = 0, total = 0;
    if (find_trailer(data, off, total)) {
        data.resize(off);
    }
    // 追加新 trailer
    const uint32_t plen = static_cast<uint32_t>(in.payload.size());
    data.insert(data.end(), kTrailerMagic, kTrailerMagic + kTrailerMagicLen);
    data.resize(data.size() + 4);
    std::memcpy(data.data() + data.size() - 4, &plen, 4);
    data.insert(data.end(), in.payload.begin(), in.payload.end());

    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return o.good();
}

} // namespace grycegc::pe
