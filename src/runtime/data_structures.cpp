#include "runtime/data_structures.h"

#include "base/binary_reader.h"
#include "base/hash.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>

namespace gm {

namespace {

void append_u32_le(std::vector<u8>& buf, u32 v) {
    buf.push_back(static_cast<u8>(v & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 8u) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 16u) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 24u) & 0xFFu));
}

void append_f64_le(std::vector<u8>& buf, f64 v) {
    u8 raw[8];
    std::memcpy(raw, &v, 8);
    buf.insert(buf.end(), raw, raw + 8);
}

void write_ds_rvalue(std::vector<u8>& buf, const RValue& val) {
    if (val.is_real()) {
        append_u32_le(buf, 0u);
        append_f64_le(buf, val.as_real());
    } else {
        append_u32_le(buf, 1u);
        const auto& s = val.as_string();
        append_u32_le(buf, static_cast<u32>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
}

Result<RValue> read_ds_rvalue(BinaryReader& r) {
    auto kind = r.read_u32();
    if (!kind) return kind.error();
    if (kind.value() == 0u) {
        auto d = r.read_f64();
        if (!d) return d.error();
        return RValue(d.value());
    }
    auto s = r.read_gm_string();
    if (!s) return s.error();
    return RValue(std::move(s.value()));
}

std::vector<u8> hex_decode(std::string_view hex) {
    auto nibble = [](char c) noexcept -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<u8> out;
    out.reserve(hex.size() / 2);
    for (usize i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<u8>((hi << 4) | lo));
    }
    return out;
}

} // namespace

void DsManager::clear_all() {
    stacks_.clear();
    queues_.clear();
    lists_.clear();
    maps_.clear();
    priorities_.clear();
    grids_.clear();
    next_stack_id_ = next_queue_id_ = next_list_id_ = next_map_id_ = next_priority_id_ = next_grid_id_ = 0;
}

bool DsManager::exists(i32 id, i32 ds_type) const noexcept {
    switch (ds_type) {
        case 1: return maps_.contains(id);
        case 2: return lists_.contains(id);
        case 3: return grids_.contains(id);
        case 4: return stacks_.contains(id);
        case 5: return queues_.contains(id);
        case 6: return priorities_.contains(id);
        default: return false;
    }
}

// ---- Stack ----
i32 DsManager::stack_create() {
    const i32 id = next_stack_id_++;
    stacks_.emplace(id, std::vector<RValue>{});
    return id;
}
void DsManager::stack_destroy(i32 id) { stacks_.erase(id); }
void DsManager::stack_clear(i32 id) { if (auto it = stacks_.find(id); it != stacks_.end()) it->second.clear(); }
void DsManager::stack_copy(i32 dst, i32 src) {
    const auto sit = stacks_.find(src);
    const auto dit = stacks_.find(dst);
    if (sit != stacks_.end() && dit != stacks_.end()) dit->second = sit->second;
}
i32 DsManager::stack_size(i32 id) const {
    const auto it = stacks_.find(id);
    return it != stacks_.end() ? static_cast<i32>(it->second.size()) : 0;
}
bool DsManager::stack_empty(i32 id) const { return stack_size(id) == 0; }
void DsManager::stack_push(i32 id, RValue val) {
    if (auto it = stacks_.find(id); it != stacks_.end()) it->second.push_back(std::move(val));
}
RValue DsManager::stack_pop(i32 id) {
    const auto it = stacks_.find(id);
    if (it == stacks_.end() || it->second.empty()) return RValue(0.0);
    RValue v = std::move(it->second.back());
    it->second.pop_back();
    return v;
}
RValue DsManager::stack_top(i32 id) const {
    const auto it = stacks_.find(id);
    return (it != stacks_.end() && !it->second.empty()) ? it->second.back() : RValue(0.0);
}
std::string DsManager::stack_write(i32 id) const {
    const auto it = stacks_.find(id);
    if (it == stacks_.end()) return {};
    std::vector<u8> buf;
    append_u32_le(buf, 101u);
    append_u32_le(buf, static_cast<u32>(it->second.size()));
    for (const auto& v : it->second) write_ds_rvalue(buf, v);
    return hex_encode(buf, true);
}
void DsManager::stack_read(i32 id, std::string_view hex) {
    const auto it = stacks_.find(id);
    if (it == stacks_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto cnt = r.read_u32();
    if (!ver || !cnt || (ver.value() != 100u && ver.value() != 101u)) return;
    it->second.clear();
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto v = read_ds_rvalue(r);
        if (!v) break;
        it->second.push_back(std::move(v.value()));
    }
}

// ---- Queue ----
i32 DsManager::queue_create() {
    const i32 id = next_queue_id_++;
    queues_.emplace(id, std::vector<RValue>{});
    return id;
}
void DsManager::queue_destroy(i32 id) { queues_.erase(id); }
void DsManager::queue_clear(i32 id) { if (auto it = queues_.find(id); it != queues_.end()) it->second.clear(); }
void DsManager::queue_copy(i32 dst, i32 src) {
    const auto sit = queues_.find(src);
    const auto dit = queues_.find(dst);
    if (sit != queues_.end() && dit != queues_.end()) dit->second = sit->second;
}
i32 DsManager::queue_size(i32 id) const {
    const auto it = queues_.find(id);
    return it != queues_.end() ? static_cast<i32>(it->second.size()) : 0;
}
bool DsManager::queue_empty(i32 id) const { return queue_size(id) == 0; }
void DsManager::queue_enqueue(i32 id, RValue val) {
    if (auto it = queues_.find(id); it != queues_.end()) it->second.push_back(std::move(val));
}
RValue DsManager::queue_dequeue(i32 id) {
    const auto it = queues_.find(id);
    if (it == queues_.end() || it->second.empty()) return RValue(0.0);
    RValue v = std::move(it->second.front());
    it->second.erase(it->second.begin());
    return v;
}
RValue DsManager::queue_head(i32 id) const {
    const auto it = queues_.find(id);
    return (it != queues_.end() && !it->second.empty()) ? it->second.front() : RValue(0.0);
}
RValue DsManager::queue_tail(i32 id) const {
    const auto it = queues_.find(id);
    return (it != queues_.end() && !it->second.empty()) ? it->second.back() : RValue(0.0);
}
std::string DsManager::queue_write(i32 id) const {
    const auto it = queues_.find(id);
    if (it == queues_.end()) return {};
    std::vector<u8> buf;
    append_u32_le(buf, 201u);
    append_u32_le(buf, static_cast<u32>(it->second.size()));
    for (const auto& v : it->second) write_ds_rvalue(buf, v);
    return hex_encode(buf, true);
}
void DsManager::queue_read(i32 id, std::string_view hex) {
    const auto it = queues_.find(id);
    if (it == queues_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto cnt = r.read_u32();
    if (!ver || !cnt || (ver.value() != 200u && ver.value() != 201u)) return;
    it->second.clear();
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto v = read_ds_rvalue(r);
        if (!v) break;
        it->second.push_back(std::move(v.value()));
    }
}

// ---- List ----
i32 DsManager::list_create() {
    const i32 id = next_list_id_++;
    lists_.emplace(id, std::vector<RValue>{});
    return id;
}
void DsManager::list_destroy(i32 id) { lists_.erase(id); }
void DsManager::list_clear(i32 id) { if (auto it = lists_.find(id); it != lists_.end()) it->second.clear(); }
void DsManager::list_copy(i32 dst, i32 src) {
    const auto sit = lists_.find(src);
    const auto dit = lists_.find(dst);
    if (sit != lists_.end() && dit != lists_.end()) dit->second = sit->second;
}
i32 DsManager::list_size(i32 id) const {
    const auto it = lists_.find(id);
    return it != lists_.end() ? static_cast<i32>(it->second.size()) : 0;
}
bool DsManager::list_empty(i32 id) const { return list_size(id) == 0; }
void DsManager::list_add(i32 id, RValue val) {
    if (auto it = lists_.find(id); it != lists_.end()) it->second.push_back(std::move(val));
}
void DsManager::list_insert(i32 id, i32 pos, RValue val) {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return;
    if (pos <= 0) it->second.insert(it->second.begin(), std::move(val));
    else if (static_cast<usize>(pos) >= it->second.size()) it->second.push_back(std::move(val));
    else it->second.insert(it->second.begin() + pos, std::move(val));
}
void DsManager::list_replace(i32 id, i32 pos, RValue val) {
    const auto it = lists_.find(id);
    if (it == lists_.end() || pos < 0 || static_cast<usize>(pos) >= it->second.size()) return;
    it->second[static_cast<usize>(pos)] = std::move(val);
}
void DsManager::list_delete(i32 id, i32 pos) {
    const auto it = lists_.find(id);
    if (it == lists_.end() || pos < 0 || static_cast<usize>(pos) >= it->second.size()) return;
    it->second.erase(it->second.begin() + pos);
}
i32 DsManager::list_find_index(i32 id, const RValue& val) const {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return -1;
    for (usize i = 0; i < it->second.size(); ++i) {
        if (RValue::compare_for_ds(it->second[i], val, precision_) == 0) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}
RValue DsManager::list_find_value(i32 id, i32 pos) const {
    const auto it = lists_.find(id);
    if (it == lists_.end() || pos < 0 || static_cast<usize>(pos) >= it->second.size()) return RValue(0.0);
    return it->second[static_cast<usize>(pos)];
}
void DsManager::list_sort(i32 id, bool ascending) {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return;
    const f64 prec = precision_;
    std::stable_sort(it->second.begin(), it->second.end(), [ascending, prec](const RValue& a, const RValue& b) {
        const i32 cmp = RValue::compare_for_ds(a, b, prec);
        return ascending ? (cmp < 0) : (cmp > 0);
    });
}
void DsManager::list_shuffle(i32 id, u32 seed) {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return;
    std::mt19937 rng(seed);
    std::shuffle(it->second.begin(), it->second.end(), rng);
}
std::string DsManager::list_write(i32 id) const {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return {};
    std::vector<u8> buf;
    append_u32_le(buf, 301u);
    append_u32_le(buf, static_cast<u32>(it->second.size()));
    for (const auto& v : it->second) write_ds_rvalue(buf, v);
    return hex_encode(buf, true);
}
void DsManager::list_read(i32 id, std::string_view hex) {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto cnt = r.read_u32();
    if (!ver || !cnt || (ver.value() != 300u && ver.value() != 301u)) return;
    it->second.clear();
    it->second.reserve(cnt.value());
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto v = read_ds_rvalue(r);
        if (!v) break;
        it->second.push_back(std::move(v.value()));
    }
}

// ---- Map (sorted parallel entries matching CDS_Map) ----
i32 DsManager::map_create() {
    const i32 id = next_map_id_++;
    maps_.emplace(id, DsMap{});
    return id;
}
void DsManager::map_destroy(i32 id) { maps_.erase(id); }
void DsManager::map_clear(i32 id) { if (auto it = maps_.find(id); it != maps_.end()) it->second.entries.clear(); }
void DsManager::map_copy(i32 dst, i32 src) {
    const auto sit = maps_.find(src);
    const auto dit = maps_.find(dst);
    if (sit != maps_.end() && dit != maps_.end()) dit->second = sit->second;
}
i32 DsManager::map_size(i32 id) const {
    const auto it = maps_.find(id);
    return it != maps_.end() ? static_cast<i32>(it->second.entries.size()) : 0;
}
bool DsManager::map_empty(i32 id) const { return map_size(id) == 0; }

void DsManager::map_add(i32 id, RValue key, RValue val) {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return;
    auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::lower_bound(vec.begin(), vec.end(), key, [prec](const auto& pair, const RValue& k) {
        return RValue::compare_for_ds(pair.first, k, prec) < 0;
    });
    if (pos != vec.end() && RValue::compare_for_ds(pos->first, key, prec) == 0) {
        return; // GM8 ds_map_add ignores duplicate key (use ds_map_replace to overwrite)
    }
    vec.emplace(pos, std::move(key), std::move(val));
}

void DsManager::map_replace(i32 id, const RValue& key, RValue val) {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return;
    auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::lower_bound(vec.begin(), vec.end(), key, [prec](const auto& pair, const RValue& k) {
        return RValue::compare_for_ds(pair.first, k, prec) < 0;
    });
    if (pos != vec.end() && RValue::compare_for_ds(pos->first, key, prec) == 0) {
        pos->second = std::move(val);
    } else {
        vec.emplace(pos, key, std::move(val));
    }
}

void DsManager::map_delete(i32 id, const RValue& key) {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return;
    auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::lower_bound(vec.begin(), vec.end(), key, [prec](const auto& pair, const RValue& k) {
        return RValue::compare_for_ds(pair.first, k, prec) < 0;
    });
    if (pos != vec.end() && RValue::compare_for_ds(pos->first, key, prec) == 0) {
        vec.erase(pos);
    }
}

bool DsManager::map_exists(i32 id, const RValue& key) const {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return false;
    const auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::lower_bound(vec.begin(), vec.end(), key, [prec](const auto& pair, const RValue& k) {
        return RValue::compare_for_ds(pair.first, k, prec) < 0;
    });
    return pos != vec.end() && RValue::compare_for_ds(pos->first, key, prec) == 0;
}

RValue DsManager::map_find_value(i32 id, const RValue& key) const {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return RValue(0.0);
    const auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::lower_bound(vec.begin(), vec.end(), key, [prec](const auto& pair, const RValue& k) {
        return RValue::compare_for_ds(pair.first, k, prec) < 0;
    });
    if (pos != vec.end() && RValue::compare_for_ds(pos->first, key, prec) == 0) {
        return pos->second;
    }
    return RValue(0.0);
}

RValue DsManager::map_find_first(i32 id) const {
    const auto it = maps_.find(id);
    if (it == maps_.end() || it->second.entries.empty()) return RValue(0.0);
    return it->second.entries.front().first;
}

RValue DsManager::map_find_last(i32 id) const {
    const auto it = maps_.find(id);
    if (it == maps_.end() || it->second.entries.empty()) return RValue(0.0);
    return it->second.entries.back().first;
}

RValue DsManager::map_find_next(i32 id, const RValue& key) const {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return RValue(0.0);
    const auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::upper_bound(vec.begin(), vec.end(), key, [prec](const RValue& k, const auto& pair) {
        return RValue::compare_for_ds(k, pair.first, prec) < 0;
    });
    return pos != vec.end() ? pos->first : RValue(0.0);
}

RValue DsManager::map_find_previous(i32 id, const RValue& key) const {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return RValue(0.0);
    const auto& vec = it->second.entries;
    const f64 prec = precision_;
    auto pos = std::lower_bound(vec.begin(), vec.end(), key, [prec](const auto& pair, const RValue& k) {
        return RValue::compare_for_ds(pair.first, k, prec) < 0;
    });
    if (pos == vec.begin()) return RValue(0.0);
    return std::prev(pos)->first;
}

std::string DsManager::map_write(i32 id) const {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return {};
    const auto& vec = it->second.entries;
    std::vector<u8> buf;
    append_u32_le(buf, 401u);
    append_u32_le(buf, static_cast<u32>(vec.size()));
    for (const auto& [k, v] : vec) { (void)v; write_ds_rvalue(buf, k); }
    for (const auto& [k, v] : vec) { (void)k; write_ds_rvalue(buf, v); }
    return hex_encode(buf, true);
}

void DsManager::map_read(i32 id, std::string_view hex) {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto cnt = r.read_u32();
    if (!ver || !cnt || (ver.value() != 400u && ver.value() != 401u)) return;

    std::vector<RValue> keys;
    keys.reserve(cnt.value());
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto k = read_ds_rvalue(r);
        if (!k) return;
        keys.push_back(std::move(k.value()));
    }
    it->second.entries.clear();
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto v = read_ds_rvalue(r);
        if (!v) break;
        map_replace(id, keys[i], std::move(v.value()));
    }
}

// ---- Priority Queue ----
i32 DsManager::priority_create() {
    const i32 id = next_priority_id_++;
    priorities_.emplace(id, DsPriority{});
    return id;
}
void DsManager::priority_destroy(i32 id) { priorities_.erase(id); }
void DsManager::priority_clear(i32 id) { if (auto it = priorities_.find(id); it != priorities_.end()) it->second.entries.clear(); }
void DsManager::priority_copy(i32 dst, i32 src) {
    const auto sit = priorities_.find(src);
    const auto dit = priorities_.find(dst);
    if (sit != priorities_.end() && dit != priorities_.end()) dit->second = sit->second;
}
i32 DsManager::priority_size(i32 id) const {
    const auto it = priorities_.find(id);
    return it != priorities_.end() ? static_cast<i32>(it->second.entries.size()) : 0;
}
bool DsManager::priority_empty(i32 id) const { return priority_size(id) == 0; }
void DsManager::priority_add(i32 id, RValue val, f64 prio) {
    if (auto it = priorities_.find(id); it != priorities_.end()) {
        it->second.entries.emplace_back(std::move(val), prio);
    }
}
void DsManager::priority_change_priority(i32 id, const RValue& val, f64 prio) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return;
    for (auto& [v, p] : it->second.entries) {
        if (RValue::compare_for_ds(v, val, precision_) == 0) {
            p = prio;
            return;
        }
    }
}
f64 DsManager::priority_find_priority(i32 id, const RValue& val) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return 0.0;
    for (const auto& [v, p] : it->second.entries) {
        if (RValue::compare_for_ds(v, val, precision_) == 0) return p;
    }
    return 0.0;
}
void DsManager::priority_delete_value(i32 id, const RValue& val) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return;
    auto& vec = it->second.entries;
    for (usize i = 0; i < vec.size(); ++i) {
        if (RValue::compare_for_ds(vec[i].first, val, precision_) == 0) {
            vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}
RValue DsManager::priority_find_min(i32 id) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    usize best = 0;
    for (usize i = 1; i < it->second.entries.size(); ++i) {
        if (it->second.entries[i].second < it->second.entries[best].second) best = i;
    }
    return it->second.entries[best].first;
}
RValue DsManager::priority_delete_min(i32 id) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    auto& vec = it->second.entries;
    usize best = 0;
    for (usize i = 1; i < vec.size(); ++i) {
        if (vec[i].second < vec[best].second) best = i;
    }
    RValue v = std::move(vec[best].first);
    vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(best));
    return v;
}
RValue DsManager::priority_find_max(i32 id) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    usize best = 0;
    for (usize i = 1; i < it->second.entries.size(); ++i) {
        if (it->second.entries[i].second > it->second.entries[best].second) best = i;
    }
    return it->second.entries[best].first;
}
RValue DsManager::priority_delete_max(i32 id) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    auto& vec = it->second.entries;
    usize best = 0;
    for (usize i = 1; i < vec.size(); ++i) {
        if (vec[i].second > vec[best].second) best = i;
    }
    RValue v = std::move(vec[best].first);
    vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(best));
    return v;
}
std::string DsManager::priority_write(i32 id) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return {};
    std::vector<u8> buf;
    append_u32_le(buf, 501u);
    append_u32_le(buf, static_cast<u32>(it->second.entries.size()));
    for (const auto& [v, p] : it->second.entries) { (void)p; write_ds_rvalue(buf, v); }
    for (const auto& [v, p] : it->second.entries) { (void)v; append_f64_le(buf, p); }
    return hex_encode(buf, true);
}
void DsManager::priority_read(i32 id, std::string_view hex) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto cnt = r.read_u32();
    if (!ver || !cnt || (ver.value() != 500u && ver.value() != 501u)) return;
    std::vector<RValue> vals;
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto v = read_ds_rvalue(r);
        if (!v) return;
        vals.push_back(std::move(v.value()));
    }
    it->second.entries.clear();
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto p = r.read_f64();
        if (!p) break;
        it->second.entries.emplace_back(std::move(vals[i]), p.value());
    }
}

// ---- Grid (column-major matching CDS_Grid) ----
i32 DsManager::grid_create(i32 w, i32 h) {
    const i32 id = next_grid_id_++;
    DsGrid g{};
    g.width = std::max(0, w);
    g.height = std::max(0, h);
    g.cells.resize(static_cast<usize>(g.width) * static_cast<usize>(g.height), RValue(0.0));
    grids_.emplace(id, std::move(g));
    return id;
}
void DsManager::grid_destroy(i32 id) { grids_.erase(id); }
void DsManager::grid_copy(i32 dst, i32 src) {
    const auto sit = grids_.find(src);
    const auto dit = grids_.find(dst);
    if (sit != grids_.end() && dit != grids_.end()) dit->second = sit->second;
}
void DsManager::grid_resize(i32 id, i32 w, i32 h) {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return;
    const i32 nw = std::max(0, w);
    const i32 nh = std::max(0, h);
    std::vector<RValue> next(static_cast<usize>(nw) * static_cast<usize>(nh), RValue(0.0));
    const i32 cw = std::min(nw, it->second.width);
    const i32 ch = std::min(nh, it->second.height);
    for (i32 x = 0; x < cw; ++x) {
        for (i32 y = 0; y < ch; ++y) {
            next[static_cast<usize>(x) * static_cast<usize>(nh) + static_cast<usize>(y)] =
                it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
        }
    }
    it->second.width = nw;
    it->second.height = nh;
    it->second.cells = std::move(next);
}
i32 DsManager::grid_width(i32 id) const {
    const auto it = grids_.find(id);
    return it != grids_.end() ? it->second.width : 0;
}
i32 DsManager::grid_height(i32 id) const {
    const auto it = grids_.find(id);
    return it != grids_.end() ? it->second.height : 0;
}
void DsManager::grid_clear(i32 id, const RValue& val) {
    if (auto it = grids_.find(id); it != grids_.end()) {
        std::fill(it->second.cells.begin(), it->second.cells.end(), val);
    }
}
void DsManager::grid_set(i32 id, i32 x, i32 y, RValue val) {
    const auto it = grids_.find(id);
    if (it == grids_.end() || x < 0 || y < 0 || x >= it->second.width || y >= it->second.height) return;
    it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)] = std::move(val);
}
void DsManager::grid_add(i32 id, i32 x, i32 y, const RValue& val) {
    const auto it = grids_.find(id);
    if (it == grids_.end() || x < 0 || y < 0 || x >= it->second.width || y >= it->second.height) return;
    auto& cell = it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
    if (auto r = RValue::add(cell, val); r) cell = std::move(r.value());
}
void DsManager::grid_multiply(i32 id, i32 x, i32 y, f64 factor) {
    const auto it = grids_.find(id);
    if (it == grids_.end() || x < 0 || y < 0 || x >= it->second.width || y >= it->second.height) return;
    auto& cell = it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
    if (cell.is_real()) cell = RValue(cell.as_real() * factor);
}
RValue DsManager::grid_get(i32 id, i32 x, i32 y) const {
    const auto it = grids_.find(id);
    if (it == grids_.end() || x < 0 || y < 0 || x >= it->second.width || y >= it->second.height) return RValue(0.0);
    return it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
}
void DsManager::grid_set_region(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val) {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    for (i32 x = x1; x <= x2; ++x) {
        for (i32 y = y1; y <= y2; ++y) {
            it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)] = val;
        }
    }
}
f64 DsManager::grid_get_sum(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    f64 sum = 0.0;
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y)
            sum += it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)].as_real();
    return sum;
}
f64 DsManager::grid_get_min(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    f64 m = std::numeric_limits<f64>::infinity();
    bool any = false;
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            m = std::min(m, it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)].as_real());
            any = true;
        }
    return any ? m : 0.0;
}
f64 DsManager::grid_get_max(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    f64 m = -std::numeric_limits<f64>::infinity();
    bool any = false;
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            m = std::max(m, it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)].as_real());
            any = true;
        }
    return any ? m : 0.0;
}
f64 DsManager::grid_get_mean(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    if (x1 > x2 || y1 > y2) return 0.0;
    const f64 cnt = static_cast<f64>((x2 - x1 + 1) * (y2 - y1 + 1));
    return grid_get_sum(id, x1, y1, x2, y2) / cnt;
}
bool DsManager::grid_value_exists(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return false;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y)
            if (RValue::compare_for_ds(it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)], val, precision_) == 0)
                return true;
    return false;
}
std::string DsManager::grid_write(i32 id) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return {};
    std::vector<u8> buf;
    append_u32_le(buf, 601u);
    append_u32_le(buf, static_cast<u32>(it->second.width));
    append_u32_le(buf, static_cast<u32>(it->second.height));
    for (const auto& cell : it->second.cells) write_ds_rvalue(buf, cell);
    return hex_encode(buf, true);
}
void DsManager::grid_read(i32 id, std::string_view hex) {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto w = r.read_i32();
    auto h = r.read_i32();
    if (!ver || !w || !h || (ver.value() != 600u && ver.value() != 601u) || w.value() < 0 || h.value() < 0) return;
    it->second.width = w.value();
    it->second.height = h.value();
    const usize total = static_cast<usize>(w.value()) * static_cast<usize>(h.value());
    it->second.cells.assign(total, RValue(0.0));
    for (usize i = 0; i < total; ++i) {
        auto v = read_ds_rvalue(r);
        if (!v) break;
        it->second.cells[i] = std::move(v.value());
    }
}

} // namespace gm
