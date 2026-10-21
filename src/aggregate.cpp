#include "flash_csv/aggregate.hpp"
#include "flash_csv/csv.hpp"
#include <algorithm>
#include <cmath>

namespace flash_csv {

static double sum_checked(double lhs, double rhs, std::uint64_t line,
                          const std::string& column) {
    // 输入有限也可能相加溢出；逐行累计与局部表合并都使用同一检查。
    const double result = lhs + rhs; // 待检查有限性的累计和。
    if (!std::isfinite(result)) throw Error(1, "sum_overflow", "non-finite sum", line, column);
    return result;
}

void merge_state(GroupState& target, const GroupState& source, const Query& query) {
    if (source.count == 0) return;
    // 在更新 count 前记下是否为空，MIN/MAX 的初值须取自第一份有效状态。
    const bool empty = target.count == 0; // 目标组在本次合并前是否为空。
    add_count(target.count, source.count);
    for (std::size_t i = 0; i < query.aggregates().size(); ++i) { // i 同时索引聚合项和状态槽位。
        const auto& agg = query.aggregates()[i]; // 当前要合并的聚合项。
        switch (agg.function) {
            case Function::Count: break;
            case Function::Sum:
            case Function::Avg:
                // AVG 合并的是累计和，最终再除以总 count，不能平均局部均值。
                target.values[i] = sum_checked(target.values[i], source.values[i], 0,
                                              query.input_header()[agg.column]);
                break;
            case Function::Min:
                target.values[i] = empty ? source.values[i] : std::min(target.values[i], source.values[i]);
                break;
            case Function::Max:
                target.values[i] = empty ? source.values[i] : std::max(target.values[i], source.values[i]);
                break;
        }
    }
}

Aggregator::Aggregator(const Query& query, std::size_t max_groups)
    : query_(query), max_groups_(max_groups) {
    if (!max_groups_) throw Error(2, "config", "max-groups must be positive");
    // 整体统计预建唯一组，因此零匹配也能输出 COUNT=0、SUM=0 的一行。
    if (!query_.group_column()) find_or_create("", 0);
}

GroupState& Aggregator::find_or_create(const std::string& key, std::uint64_t line) {
    const auto found = groups_.find(key); // 查找已有组，避免重复创建状态。
    if (found != groups_.end()) return found->second;
    // 只在创建新组时检查容量；局部表和最终合并表分别受同一上限约束。
    if (groups_.size() == max_groups_) {
        const auto column = query_.group_column(); // 可选的分组字段下标，供容量错误定位。
        throw Error(1, "group_limit", "table exceeds max-groups", line,
                    column ? query_.input_header()[*column] : "");
    }
    return groups_.emplace(key, GroupState{0, std::vector<double>(query_.aggregates().size(), 0)}).first->second;
}

void Aggregator::consume(const Row& row) {
    // 先过滤再读取聚合数值，被排除的行无需满足聚合列的数值语法。
    if (!query_.matches(row)) return;
    const auto column = query_.group_column(); // 有值时按该字段分组，否则归入整体统计组。
    auto& state = find_or_create(column ? row.fields[*column] : "", row.line); // 当前记录所属组的状态。
    add_count(state.count, 1, row.line);
    add_count(matched_, 1, row.line);
    for (std::size_t i = 0; i < query_.aggregates().size(); ++i) { // i 对应 values 中的统计槽位。
        const auto& agg = query_.aggregates()[i]; // 本轮要更新的聚合项。
        if (agg.function == Function::Count) continue;
        const auto& name = query_.input_header()[agg.column]; // 数值错误中显示的原始列名。
        const double value = parse_number(row.fields[agg.column], row.line, name); // 当前行的聚合数值。
        switch (agg.function) {
            case Function::Count: break;
            case Function::Sum:
            case Function::Avg: state.values[i] = sum_checked(state.values[i], value, row.line, name); break;
            case Function::Min:
                // 零初值不参与极值比较；首行同时适用于全正或全负的数据。
                state.values[i] = state.count == 1 ? value : std::min(state.values[i], value); break;
            case Function::Max:
                state.values[i] = state.count == 1 ? value : std::max(state.values[i], value); break;
        }
    }
}

void Aggregator::merge(const Aggregator& other) {
    for (const auto& entry : other.groups_) // first 是分组键，second 是该组统计状态。
        merge_state(find_or_create(entry.first, 0), entry.second, query_);
    add_count(matched_, other.matched_);
}

std::uint64_t Aggregator::write(std::ostream& output) const {
    using Entry = decltype(groups_)::value_type;
    // 哈希表遍历无序，排序条目指针可稳定输出且不用复制组键或状态。
    std::vector<const Entry*> sorted; // 排序后的组条目指针，只借用哈希表内容。
    sorted.reserve(groups_.size());
    for (const auto& entry : groups_) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const Entry* lhs, const Entry* rhs) {
        // 明确使用 unsigned char，避免 char 的符号性改变非 ASCII 字节顺序。
        return std::lexicographical_compare(lhs->first.begin(), lhs->first.end(),
                                            rhs->first.begin(), rhs->first.end(),
                                            [](unsigned char a, unsigned char b) { return a < b; });
    });
    std::uint64_t rows = 0; // 已写出的统计数据行数。
    for (const auto* entry : sorted) {
        const auto& state = entry->second; // 当前输出组的最终统计状态。
        std::vector<std::string> fields; // 当前输出行：可选组键及各聚合结果。
        if (query_.group_column()) fields.push_back(entry->first);
        for (std::size_t i = 0; i < query_.aggregates().size(); ++i) { // i 为当前聚合结果下标。
            const auto function = query_.aggregates()[i].function; // 决定状态如何转换为输出值。
            if (function == Function::Count) fields.push_back(std::to_string(state.count));
            // 空组的 SUM 为 0，MIN/MAX/AVG 留空；也避免 AVG 除以零。
            else if (!state.count && function != Function::Sum) fields.emplace_back();
            else fields.push_back(format_number(function == Function::Avg ?
                                                state.values[i] / static_cast<double>(state.count) : state.values[i]));
        }
        write_record(output, fields);
        add_count(rows, 1);
    }
    return rows;
}

} // namespace flash_csv
