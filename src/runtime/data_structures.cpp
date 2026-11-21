#include "runtime/data_structures.h"

#include "base/binary_reader.h"
#include "base/hash.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace gm {

namespace {

void append_u32_le(std::vector<u8>& buf, u32 v) {
    buf.push_back(static_cast<u8>(v & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 8u) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 16u) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 24u) & 0xFFu));
}

void append_f64_le(std::vector<u8>& buf, f64 v) {
    const u64 bits = std::bit_cast<u64>(v);
    append_u32_le(buf, static_cast<u32>(bits));
    append_u32_le(buf, static_cast<u32>(bits >> 32u));
}

// Each GM8 DS value stores its tag, real, and length-prefixed string, in that order.
constexpr usize kDsValueHeaderSize = sizeof(u32) + sizeof(f64) + sizeof(u32);

// GM8.0 writes byte strings verbatim. GM8.1's Unicode stream conversion
// replaces malformed UTF-8 without changing the string held by the container.
bool repair_stream_utf8(std::string_view bytes, EncodingMode encoding, std::string& repaired) {
    if (encoding != EncodingMode::Utf8) return false;
    usize valid_start = 0;
    for (usize pos = 0; pos < bytes.size();) {
        const usize start = pos;
        const u8 lead = static_cast<u8>(bytes[pos++]);
        if (lead < 0x80) continue;
        if (lead >= 0xC2 && lead <= 0xF4) {
            const usize length = lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4);
            while (pos - start < length && pos < bytes.size()) {
                const u8 next = static_cast<u8>(bytes[pos]);
                if ((next & 0xC0) != 0x80) break;
                ++pos;
                // The native decoder consumes the second byte even when it
                // makes the sequence overlong, a surrogate, or above U+10FFFF.
                if (pos - start == 2 &&
                    ((lead == 0xE0 && next < 0xA0) || (lead == 0xED && next >= 0xA0) ||
                     (lead == 0xF0 && next < 0x90) || (lead == 0xF4 && next >= 0x90))) break;
            }
            if (pos - start == length) continue;
        }
        repaired.append(bytes.substr(valid_start, start - valid_start));
        repaired.append("\xEF\xBF\xBD");
        valid_start = pos;
    }
    if (valid_start == 0) return false;
    repaired.append(bytes.substr(valid_start));
    return true;
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

void DsManager::write_ds_rvalue(std::vector<u8>& buf, const RValue& val) const {
    append_u32_le(buf, val.is_string() ? 1u : 0u);
    append_f64_le(buf, val.as_real());
    std::string repaired;
    std::string_view text = val.as_string();
    if (repair_stream_utf8(text, string_encoding_, repaired)) text = repaired;
    append_u32_le(buf, static_cast<u32>(text.size()));
    buf.insert(buf.end(), text.begin(), text.end());
}

Result<RValue> DsManager::read_ds_rvalue(BinaryReader& r) const {
    auto kind = r.read_u32();
    if (!kind) return kind.error();
    if (kind.value() > 1u) return Error{"Unknown DS value type"};
    auto number = r.read_f64();
    if (!number) return number.error();
    auto text = r.read_gm_string();
    if (!text) return text.error();
    std::string repaired;
    if (repair_stream_utf8(text.value(), string_encoding_, repaired)) text.value() = std::move(repaired);
    return RValue(static_cast<RValue::Kind>(kind.value()), number.value(), std::move(text.value()));
}

bool DsManager::read_ds_values(BinaryReader& r, u64 count, std::vector<RValue>& values) const {
    if (count > r.remaining() / kDsValueHeaderSize) return false;
    std::vector<RValue> parsed;
    parsed.reserve(static_cast<usize>(count));
    for (usize i = 0; i < count; ++i) {
        auto value = read_ds_rvalue(r);
        if (!value) return false;
        parsed.push_back(std::move(value.value()));
    }
    values = std::move(parsed);
    return true;
}

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
    if (ver && ver.value() == 101u && cnt) {
        read_ds_values(r, cnt.value(), it->second);
    }
}

// ---- Queue ----
i32 DsManager::queue_create() {
    const i32 id = next_queue_id_++;
    queues_.try_emplace(id);
    return id;
}
void DsManager::queue_destroy(i32 id) { queues_.erase(id); }
void DsManager::queue_clear(i32 id) { if (auto it = queues_.find(id); it != queues_.end()) it->second = {}; }
void DsManager::queue_copy(i32 dst, i32 src) {
    const auto sit = queues_.find(src);
    const auto dit = queues_.find(dst);
    if (sit != queues_.end() && dit != queues_.end()) dit->second = sit->second;
}
i32 DsManager::queue_size(i32 id) const {
    const auto it = queues_.find(id);
    return it != queues_.end() ? static_cast<i32>(it->second.write_pos - it->second.read_pos) : 0;
}
bool DsManager::queue_empty(i32 id) const { return queue_size(id) == 0; }
void DsManager::queue_enqueue(i32 id, RValue val) {
    const auto it = queues_.find(id);
    if (it == queues_.end()) return;
    auto& queue = it->second;
    if (queue.write_pos == queue.slots.size()) queue.slots.resize(queue.write_pos + 16);
    queue.slots[queue.write_pos++] = std::move(val);
}
RValue DsManager::queue_dequeue(i32 id) {
    const auto it = queues_.find(id);
    if (it == queues_.end()) return RValue(0.0);
    auto& queue = it->second;
    RValue value;
    if (queue.read_pos < queue.write_pos) value = queue.slots[queue.read_pos++];
    // GM copies the remaining entries forward and keeps the old slots. They
    // are observable in ds_queue_write, including strings already dequeued.
    if (queue.read_pos > 16 && queue.read_pos > queue.write_pos / 2) {
        queue.write_pos -= queue.read_pos;
        for (usize i = 0; i < queue.write_pos; ++i) queue.slots[i] = queue.slots[queue.read_pos + i];
        queue.read_pos = 0;
    }
    return value;
}
RValue DsManager::queue_head(i32 id) const {
    const auto it = queues_.find(id);
    if (it == queues_.end() || it->second.read_pos == it->second.write_pos) return RValue(0.0);
    return it->second.slots[it->second.read_pos];
}
RValue DsManager::queue_tail(i32 id) const {
    const auto it = queues_.find(id);
    if (it == queues_.end() || it->second.read_pos == it->second.write_pos) return RValue(0.0);
    return it->second.slots[it->second.write_pos - 1];
}
std::string DsManager::queue_write(i32 id) const {
    const auto it = queues_.find(id);
    if (it == queues_.end()) return {};
    const auto& queue = it->second;
    std::vector<u8> buf;
    append_u32_le(buf, 201u);
    append_u32_le(buf, static_cast<u32>(queue.write_pos));
    append_u32_le(buf, static_cast<u32>(queue.read_pos));
    append_u32_le(buf, static_cast<u32>(queue.slots.size()));
    for (const auto& value : queue.slots) write_ds_rvalue(buf, value);
    return hex_encode(buf, true);
}
void DsManager::queue_read(i32 id, std::string_view hex) {
    const auto it = queues_.find(id);
    if (it == queues_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto write_pos = r.read_u32();
    auto read_pos = r.read_u32();
    auto capacity = r.read_u32();
    if (!ver || ver.value() != 201u || !write_pos || !read_pos || !capacity) return;
    if (read_pos.value() > write_pos.value() || write_pos.value() > capacity.value()) return;
    Queue queue;
    queue.read_pos = read_pos.value();
    queue.write_pos = write_pos.value();
    if (!read_ds_values(r, capacity.value(), queue.slots)) return;
    it->second = std::move(queue);
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
void DsManager::list_shuffle(i32 id, GmlRandom& rng) {
    const auto it = lists_.find(id);
    if (it == lists_.end()) return;
    auto& values = it->second;
    const auto count = static_cast<u32>(values.size());
    for (usize i = 0; i < values.size() * 8; ++i) {
        const u32 first = rng.below(count);
        const u32 second = rng.below(count);
        std::swap(values[first], values[second]);
    }
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
    if (ver && ver.value() == 301u && cnt) {
        read_ds_values(r, cnt.value(), it->second);
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
    auto pos = std::upper_bound(vec.begin(), vec.end(), key, [prec](const RValue& k, const auto& pair) {
        return RValue::compare_for_ds(k, pair.first, prec) < 0;
    });
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
    for (const auto& entry : vec) write_ds_rvalue(buf, entry.first);
    for (const auto& entry : vec) write_ds_rvalue(buf, entry.second);
    return hex_encode(buf, true);
}

void DsManager::map_read(i32 id, std::string_view hex) {
    const auto it = maps_.find(id);
    if (it == maps_.end()) return;
    auto bytes = hex_decode(hex);
    BinaryReader r(bytes);
    auto ver = r.read_u32();
    auto cnt = r.read_u32();
    if (!ver || ver.value() != 401u || !cnt || cnt.value() > r.remaining() / (2 * kDsValueHeaderSize)) return;
    DsMap parsed;
    parsed.entries.reserve(cnt.value());
    for (u32 i = 0; i < cnt.value(); ++i) {
        auto key = read_ds_rvalue(r);
        if (!key) return;
        parsed.entries.emplace_back(std::move(key.value()), RValue{});
    }
    for (auto& entry : parsed.entries) {
        auto value = read_ds_rvalue(r);
        if (!value) return;
        entry.second = std::move(value.value());
    }
    it->second = std::move(parsed);
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
void DsManager::priority_add(i32 id, RValue val, RValue prio) {
    if (auto it = priorities_.find(id); it != priorities_.end()) {
        it->second.entries.push_back({std::move(val), std::move(prio)});
    }
}
void DsManager::priority_change_priority(i32 id, const RValue& val, RValue prio) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return;
    for (auto& entry : it->second.entries) {
        if (RValue::compare_for_ds(entry.value, val, precision_) == 0) {
            entry.priority = std::move(prio);
            return;
        }
    }
}
RValue DsManager::priority_find_priority(i32 id, const RValue& val) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return RValue(0.0);
    for (const auto& entry : it->second.entries) {
        if (RValue::compare_for_ds(entry.value, val, precision_) == 0) return entry.priority;
    }
    return RValue(0.0);
}

usize DsPriority::extreme_index(bool maximum, f64 precision) const {
    usize best = 0;
    for (usize i = 1; i < entries.size(); ++i) {
        const i32 cmp = RValue::compare_for_ds(entries[i].priority, entries[best].priority, precision);
        if (maximum ? cmp > 0 : cmp < 0) best = i;
    }
    return best;
}

RValue DsPriority::remove_at(usize index) {
    RValue value = std::move(entries[index].value);
    // The runner fills the vacancy with the last entry. This also affects
    // which value wins the next tie and the order in ds_priority_write.
    if (index != entries.size() - 1) entries[index] = std::move(entries.back());
    entries.pop_back();
    return value;
}

void DsManager::priority_delete_value(i32 id, const RValue& val) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return;
    auto& queue = it->second;
    for (usize i = 0; i < queue.entries.size(); ++i) {
        if (RValue::compare_for_ds(queue.entries[i].value, val, precision_) == 0) {
            queue.remove_at(i);
            return;
        }
    }
}
RValue DsManager::priority_find_min(i32 id) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    const auto& queue = it->second;
    return queue.entries[queue.extreme_index(false, precision_)].value;
}
RValue DsManager::priority_delete_min(i32 id) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    auto& queue = it->second;
    return queue.remove_at(queue.extreme_index(false, precision_));
}
RValue DsManager::priority_find_max(i32 id) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    const auto& queue = it->second;
    return queue.entries[queue.extreme_index(true, precision_)].value;
}
RValue DsManager::priority_delete_max(i32 id) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end() || it->second.entries.empty()) return RValue(0.0);
    auto& queue = it->second;
    return queue.remove_at(queue.extreme_index(true, precision_));
}
std::string DsManager::priority_write(i32 id) const {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return {};
    const auto& entries = it->second.entries;
    std::vector<u8> buf;
    append_u32_le(buf, 501u);
    append_u32_le(buf, static_cast<u32>(entries.size()));
    for (const auto& entry : entries) write_ds_rvalue(buf, entry.priority);
    for (const auto& entry : entries) write_ds_rvalue(buf, entry.value);
    return hex_encode(buf, true);
}
void DsManager::priority_read(i32 id, std::string_view hex) {
    const auto it = priorities_.find(id);
    if (it == priorities_.end()) return;
    const auto bytes = hex_decode(hex);
    BinaryReader reader(bytes);
    const auto version = reader.read_u32();
    const auto count = reader.read_u32();
    if (!version || version.value() != 501u || !count ||
        count.value() > reader.remaining() / (2 * kDsValueHeaderSize)) return;

    DsPriority restored;
    restored.entries.resize(count.value());
    for (auto& entry : restored.entries) {
        auto priority = read_ds_rvalue(reader);
        if (!priority) return;
        entry.priority = std::move(priority.value());
    }
    for (auto& entry : restored.entries) {
        auto value = read_ds_rvalue(reader);
        if (!value) return;
        entry.value = std::move(value.value());
    }
    it->second = std::move(restored);
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
void DsManager::grid_add_region(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val) {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            auto& cell = it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
            if (auto r = RValue::add(cell, val); r) cell = std::move(r.value());
        }
}
void DsManager::grid_multiply_region(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, f64 factor) {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            auto& cell = it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
            if (cell.is_real()) cell = RValue(cell.as_real() * factor);
        }
}
void DsManager::grid_set_disk(i32 id, f64 xm, f64 ym, f64 r, const RValue& val) {
    const auto it = grids_.find(id);
    if (it == grids_.end() || r < 0.0) return;
    const f64 r2 = r * r;
    const i32 x1 = std::max(0, static_cast<i32>(std::floor(xm - r)));
    const i32 x2 = std::min(it->second.width - 1, static_cast<i32>(std::ceil(xm + r)));
    const i32 y1 = std::max(0, static_cast<i32>(std::floor(ym - r)));
    const i32 y2 = std::min(it->second.height - 1, static_cast<i32>(std::ceil(ym + r)));
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            const f64 dx = static_cast<f64>(x) - xm, dy = static_cast<f64>(y) - ym;
            if (dx * dx + dy * dy <= r2)
                it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)] = val;
        }
}
void DsManager::grid_add_disk(i32 id, f64 xm, f64 ym, f64 r, const RValue& val) {
    const auto it = grids_.find(id);
    if (it == grids_.end() || r < 0.0) return;
    const f64 r2 = r * r;
    const i32 x1 = std::max(0, static_cast<i32>(std::floor(xm - r)));
    const i32 x2 = std::min(it->second.width - 1, static_cast<i32>(std::ceil(xm + r)));
    const i32 y1 = std::max(0, static_cast<i32>(std::floor(ym - r)));
    const i32 y2 = std::min(it->second.height - 1, static_cast<i32>(std::ceil(ym + r)));
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            const f64 dx = static_cast<f64>(x) - xm, dy = static_cast<f64>(y) - ym;
            if (dx * dx + dy * dy <= r2) {
                auto& cell = it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
                if (auto res = RValue::add(cell, val); res) cell = std::move(res.value());
            }
        }
}
void DsManager::grid_multiply_disk(i32 id, f64 xm, f64 ym, f64 r, f64 factor) {
    const auto it = grids_.find(id);
    if (it == grids_.end() || r < 0.0) return;
    const f64 r2 = r * r;
    const i32 x1 = std::max(0, static_cast<i32>(std::floor(xm - r)));
    const i32 x2 = std::min(it->second.width - 1, static_cast<i32>(std::ceil(xm + r)));
    const i32 y1 = std::max(0, static_cast<i32>(std::floor(ym - r)));
    const i32 y2 = std::min(it->second.height - 1, static_cast<i32>(std::ceil(ym + r)));
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            const f64 dx = static_cast<f64>(x) - xm, dy = static_cast<f64>(y) - ym;
            if (dx * dx + dy * dy <= r2) {
                auto& cell = it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)];
                if (cell.is_real()) cell = RValue(cell.as_real() * factor);
            }
        }
}
void DsManager::grid_set_grid_region(i32 id, i32 src_id, i32 x1, i32 y1, i32 x2, i32 y2, i32 xpos, i32 ypos) {
    const auto dit = grids_.find(id);
    const auto sit = grids_.find(src_id);
    if (dit == grids_.end() || sit == grids_.end()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    const DsGrid src_copy = sit->second;
    for (i32 sx = x1; sx <= x2; ++sx) {
        for (i32 sy = y1; sy <= y2; ++sy) {
            if (sx < 0 || sy < 0 || sx >= src_copy.width || sy >= src_copy.height) continue;
            const i32 dx = xpos + (sx - x1), dy = ypos + (sy - y1);
            if (dx < 0 || dy < 0 || dx >= dit->second.width || dy >= dit->second.height) continue;
            dit->second.cells[static_cast<usize>(dx) * static_cast<usize>(dit->second.height) + static_cast<usize>(dy)] =
                src_copy.cells[static_cast<usize>(sx) * static_cast<usize>(src_copy.height) + static_cast<usize>(sy)];
        }
    }
}
void DsManager::grid_add_grid_region(i32 id, i32 src_id, i32 x1, i32 y1, i32 x2, i32 y2, i32 xpos, i32 ypos) {
    const auto dit = grids_.find(id);
    const auto sit = grids_.find(src_id);
    if (dit == grids_.end() || sit == grids_.end()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    const DsGrid src_copy = sit->second;
    for (i32 sx = x1; sx <= x2; ++sx) {
        for (i32 sy = y1; sy <= y2; ++sy) {
            if (sx < 0 || sy < 0 || sx >= src_copy.width || sy >= src_copy.height) continue;
            const i32 dx = xpos + (sx - x1), dy = ypos + (sy - y1);
            if (dx < 0 || dy < 0 || dx >= dit->second.width || dy >= dit->second.height) continue;
            auto& dst_cell = dit->second.cells[static_cast<usize>(dx) * static_cast<usize>(dit->second.height) + static_cast<usize>(dy)];
            const auto& src_cell = src_copy.cells[static_cast<usize>(sx) * static_cast<usize>(src_copy.height) + static_cast<usize>(sy)];
            if (auto r = RValue::add(dst_cell, src_cell); r) dst_cell = std::move(r.value());
        }
    }
}
void DsManager::grid_multiply_grid_region(i32 id, i32 src_id, i32 x1, i32 y1, i32 x2, i32 y2, i32 xpos, i32 ypos) {
    const auto dit = grids_.find(id);
    const auto sit = grids_.find(src_id);
    if (dit == grids_.end() || sit == grids_.end()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    const DsGrid src_copy = sit->second;
    for (i32 sx = x1; sx <= x2; ++sx) {
        for (i32 sy = y1; sy <= y2; ++sy) {
            if (sx < 0 || sy < 0 || sx >= src_copy.width || sy >= src_copy.height) continue;
            const i32 dx = xpos + (sx - x1), dy = ypos + (sy - y1);
            if (dx < 0 || dy < 0 || dx >= dit->second.width || dy >= dit->second.height) continue;
            auto& dst_cell = dit->second.cells[static_cast<usize>(dx) * static_cast<usize>(dit->second.height) + static_cast<usize>(dy)];
            const auto& src_cell = src_copy.cells[static_cast<usize>(sx) * static_cast<usize>(src_copy.height) + static_cast<usize>(sy)];
            if (dst_cell.is_real() && src_cell.is_real()) dst_cell = RValue(dst_cell.as_real() * src_cell.as_real());
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
    return grid_value_x(id, x1, y1, x2, y2, val) >= 0;
}
i32 DsManager::grid_value_x(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return -1;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y)
            if (RValue::compare_for_ds(it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)], val, precision_) == 0)
                return x;
    return -1;
}
i32 DsManager::grid_value_y(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return -1;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(it->second.width - 1, x2); y2 = std::min(it->second.height - 1, y2);
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y)
            if (RValue::compare_for_ds(it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)], val, precision_) == 0)
                return y;
    return -1;
}
namespace {
template <typename Visit>
void for_each_disk_number(const DsGrid& grid, f64 xm, f64 ym, f64 radius, Visit visit) {
    if (grid.width <= 0 || grid.height <= 0) return;
    const f64 left = std::max(0.0, std::floor(xm - radius));
    const f64 right = std::min(static_cast<f64>(grid.width - 1), std::ceil(xm + radius));
    const f64 top = std::max(0.0, std::floor(ym - radius));
    const f64 bottom = std::min(static_cast<f64>(grid.height - 1), std::ceil(ym + radius));
    if (left > right || top > bottom) return;

    const f64 radius_squared = radius * radius;
    for (i32 x = static_cast<i32>(left); x <= static_cast<i32>(right); ++x) {
        const f64 dx = static_cast<f64>(x) - xm;
        for (i32 y = static_cast<i32>(top); y <= static_cast<i32>(bottom); ++y) {
            const f64 dy = static_cast<f64>(y) - ym;
            const auto& cell = grid.cells[static_cast<usize>(x) * grid.height + y];
            if (dx * dx + dy * dy <= radius_squared && cell.is_real()) visit(cell.as_real());
        }
    }
}
} // namespace

f64 DsManager::grid_get_disk_sum(i32 id, f64 xm, f64 ym, f64 r) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    f64 sum = 0.0;
    for_each_disk_number(it->second, xm, ym, r, [&](f64 value) { sum += value; });
    return sum;
}
f64 DsManager::grid_get_disk_min(i32 id, f64 xm, f64 ym, f64 r) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    f64 minimum = 1e8; // GM8 accumulator, including when the disk contains no numbers.
    for_each_disk_number(it->second, xm, ym, r, [&](f64 value) { minimum = std::min(minimum, value); });
    return minimum;
}
f64 DsManager::grid_get_disk_max(i32 id, f64 xm, f64 ym, f64 r) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    f64 maximum = -1e8;
    for_each_disk_number(it->second, xm, ym, r, [&](f64 value) { maximum = std::max(maximum, value); });
    return maximum;
}
f64 DsManager::grid_get_disk_mean(i32 id, f64 xm, f64 ym, f64 r) const {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return 0.0;
    f64 sum = 0.0;
    usize count = 0;
    for_each_disk_number(it->second, xm, ym, r, [&](f64 value) {
        sum += value;
        ++count;
    });
    return count ? sum / static_cast<f64>(count) : 0.0;
}
bool DsManager::grid_value_disk_exists(i32 id, f64 xm, f64 ym, f64 r, const RValue& val) const {
    return grid_value_disk_x(id, xm, ym, r, val) >= 0;
}
i32 DsManager::grid_value_disk_x(i32 id, f64 xm, f64 ym, f64 r, const RValue& val) const {
    const auto it = grids_.find(id);
    if (it == grids_.end() || r < 0.0) return -1;
    const f64 r2 = r * r;
    const i32 x1 = std::max(0, static_cast<i32>(std::floor(xm - r)));
    const i32 x2 = std::min(it->second.width - 1, static_cast<i32>(std::ceil(xm + r)));
    const i32 y1 = std::max(0, static_cast<i32>(std::floor(ym - r)));
    const i32 y2 = std::min(it->second.height - 1, static_cast<i32>(std::ceil(ym + r)));
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            const f64 dx = static_cast<f64>(x) - xm, dy = static_cast<f64>(y) - ym;
            if (dx * dx + dy * dy <= r2 &&
                RValue::compare_for_ds(it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)], val, precision_) == 0)
                return x;
        }
    return -1;
}
i32 DsManager::grid_value_disk_y(i32 id, f64 xm, f64 ym, f64 r, const RValue& val) const {
    const auto it = grids_.find(id);
    if (it == grids_.end() || r < 0.0) return -1;
    const f64 r2 = r * r;
    const i32 x1 = std::max(0, static_cast<i32>(std::floor(xm - r)));
    const i32 x2 = std::min(it->second.width - 1, static_cast<i32>(std::ceil(xm + r)));
    const i32 y1 = std::max(0, static_cast<i32>(std::floor(ym - r)));
    const i32 y2 = std::min(it->second.height - 1, static_cast<i32>(std::ceil(ym + r)));
    for (i32 x = x1; x <= x2; ++x)
        for (i32 y = y1; y <= y2; ++y) {
            const f64 dx = static_cast<f64>(x) - xm, dy = static_cast<f64>(y) - ym;
            if (dx * dx + dy * dy <= r2 &&
                RValue::compare_for_ds(it->second.cells[static_cast<usize>(x) * static_cast<usize>(it->second.height) + static_cast<usize>(y)], val, precision_) == 0)
                return y;
        }
    return -1;
}
void DsManager::grid_shuffle(i32 id, GmlRandom& rng) {
    const auto it = grids_.find(id);
    if (it == grids_.end()) return;
    auto& grid = it->second;
    const auto width = static_cast<u32>(grid.width);
    const auto height = static_cast<u32>(grid.height);
    for (usize i = 0; i < grid.cells.size() * 8; ++i) {
        const usize x1 = rng.below(width);
        const usize x2 = rng.below(width);
        const usize y1 = rng.below(height);
        const usize y2 = rng.below(height);
        std::swap(grid.cells[x1 * height + y1], grid.cells[x2 * height + y2]);
    }
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
    if (!ver || ver.value() != 601u || !w || !h || w.value() < 0 || h.value() < 0) return;
    const u64 total = static_cast<u64>(w.value()) * static_cast<u64>(h.value());
    if (read_ds_values(r, total, it->second.cells)) {
        it->second.width = w.value();
        it->second.height = h.value();
    }
}

} // namespace gm
