#pragma once

#include "base/math.h"
#include "base/types.h"
#include "gml/value.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gm {

struct DsGrid {
    i32 width = 0;
    i32 height = 0;
    std::vector<RValue> cells; // column-major x * height + y (matching GM8)
};

struct DsMap {
    // Kept sorted by RValue::compare_for_ds(key, ...) matching Delphi CDS_Map!
    std::vector<std::pair<RValue, RValue>> entries;
};

struct DsPriority {
    std::vector<std::pair<RValue, f64>> entries;
};

class DsManager {
public:
    void clear_all();
    void set_precision(f64 prec) noexcept { precision_ = prec; }
    [[nodiscard]] f64 precision() const noexcept { return precision_; }

    [[nodiscard]] bool exists(i32 id, i32 ds_type) const noexcept;

    // Stack (ds_type_stack = 4)
    i32 stack_create();
    void stack_destroy(i32 id);
    void stack_clear(i32 id);
    void stack_copy(i32 dst, i32 src);
    [[nodiscard]] i32 stack_size(i32 id) const;
    [[nodiscard]] bool stack_empty(i32 id) const;
    void stack_push(i32 id, RValue val);
    RValue stack_pop(i32 id);
    [[nodiscard]] RValue stack_top(i32 id) const;
    [[nodiscard]] std::string stack_write(i32 id) const;
    void stack_read(i32 id, std::string_view hex);

    // Queue (ds_type_queue = 5)
    i32 queue_create();
    void queue_destroy(i32 id);
    void queue_clear(i32 id);
    void queue_copy(i32 dst, i32 src);
    [[nodiscard]] i32 queue_size(i32 id) const;
    [[nodiscard]] bool queue_empty(i32 id) const;
    void queue_enqueue(i32 id, RValue val);
    RValue queue_dequeue(i32 id);
    [[nodiscard]] RValue queue_head(i32 id) const;
    [[nodiscard]] RValue queue_tail(i32 id) const;
    [[nodiscard]] std::string queue_write(i32 id) const;
    void queue_read(i32 id, std::string_view hex);

    // List (ds_type_list = 2)
    i32 list_create();
    void list_destroy(i32 id);
    void list_clear(i32 id);
    void list_copy(i32 dst, i32 src);
    [[nodiscard]] i32 list_size(i32 id) const;
    [[nodiscard]] bool list_empty(i32 id) const;
    void list_add(i32 id, RValue val);
    void list_insert(i32 id, i32 pos, RValue val);
    void list_replace(i32 id, i32 pos, RValue val);
    void list_delete(i32 id, i32 pos);
    [[nodiscard]] i32 list_find_index(i32 id, const RValue& val) const;
    [[nodiscard]] RValue list_find_value(i32 id, i32 pos) const;
    void list_sort(i32 id, bool ascending);
    void list_shuffle(i32 id, u32 seed);
    [[nodiscard]] std::string list_write(i32 id) const;
    void list_read(i32 id, std::string_view hex);

    // Map (ds_type_map = 1)
    i32 map_create();
    void map_destroy(i32 id);
    void map_clear(i32 id);
    void map_copy(i32 dst, i32 src);
    [[nodiscard]] i32 map_size(i32 id) const;
    [[nodiscard]] bool map_empty(i32 id) const;
    void map_add(i32 id, RValue key, RValue val);
    void map_replace(i32 id, const RValue& key, RValue val);
    void map_delete(i32 id, const RValue& key);
    [[nodiscard]] bool map_exists(i32 id, const RValue& key) const;
    [[nodiscard]] RValue map_find_value(i32 id, const RValue& key) const;
    [[nodiscard]] RValue map_find_first(i32 id) const;
    [[nodiscard]] RValue map_find_last(i32 id) const;
    [[nodiscard]] RValue map_find_next(i32 id, const RValue& key) const;
    [[nodiscard]] RValue map_find_previous(i32 id, const RValue& key) const;
    [[nodiscard]] std::string map_write(i32 id) const;
    void map_read(i32 id, std::string_view hex);

    // Priority Queue (ds_type_priority = 6)
    i32 priority_create();
    void priority_destroy(i32 id);
    void priority_clear(i32 id);
    void priority_copy(i32 dst, i32 src);
    [[nodiscard]] i32 priority_size(i32 id) const;
    [[nodiscard]] bool priority_empty(i32 id) const;
    void priority_add(i32 id, RValue val, f64 prio);
    void priority_change_priority(i32 id, const RValue& val, f64 prio);
    [[nodiscard]] f64 priority_find_priority(i32 id, const RValue& val) const;
    void priority_delete_value(i32 id, const RValue& val);
    RValue priority_delete_min(i32 id);
    [[nodiscard]] RValue priority_find_min(i32 id) const;
    RValue priority_delete_max(i32 id);
    [[nodiscard]] RValue priority_find_max(i32 id) const;
    [[nodiscard]] std::string priority_write(i32 id) const;
    void priority_read(i32 id, std::string_view hex);

    // Grid (ds_type_grid = 3)
    i32 grid_create(i32 w, i32 h);
    void grid_destroy(i32 id);
    void grid_copy(i32 dst, i32 src);
    void grid_resize(i32 id, i32 w, i32 h);
    [[nodiscard]] i32 grid_width(i32 id) const;
    [[nodiscard]] i32 grid_height(i32 id) const;
    void grid_clear(i32 id, const RValue& val);
    void grid_set(i32 id, i32 x, i32 y, RValue val);
    void grid_add(i32 id, i32 x, i32 y, const RValue& val);
    void grid_multiply(i32 id, i32 x, i32 y, f64 factor);
    [[nodiscard]] RValue grid_get(i32 id, i32 x, i32 y) const;
    void grid_set_region(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val);
    [[nodiscard]] f64 grid_get_sum(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const;
    [[nodiscard]] f64 grid_get_min(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const;
    [[nodiscard]] f64 grid_get_max(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const;
    [[nodiscard]] f64 grid_get_mean(i32 id, i32 x1, i32 y1, i32 x2, i32 y2) const;
    [[nodiscard]] bool grid_value_exists(i32 id, i32 x1, i32 y1, i32 x2, i32 y2, const RValue& val) const;
    [[nodiscard]] std::string grid_write(i32 id) const;
    void grid_read(i32 id, std::string_view hex);

private:
    f64 precision_ = kDefaultDsPrecision;

    std::unordered_map<i32, std::vector<RValue>> stacks_;
    std::unordered_map<i32, std::vector<RValue>> queues_;
    std::unordered_map<i32, std::vector<RValue>> lists_;
    std::unordered_map<i32, DsMap> maps_;
    std::unordered_map<i32, DsPriority> priorities_;
    std::unordered_map<i32, DsGrid> grids_;

    i32 next_stack_id_ = 0;
    i32 next_queue_id_ = 0;
    i32 next_list_id_ = 0;
    i32 next_map_id_ = 0;
    i32 next_priority_id_ = 0;
    i32 next_grid_id_ = 0;
};

} // namespace gm
