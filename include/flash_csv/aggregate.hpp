#pragma once

#include "flash_csv/query.hpp"
#include <ostream>
#include <unordered_map>

namespace flash_csv {

// GroupState（分组状态）：保存单个分组的匹配行数和各聚合项的累计值。
// count 是该组匹配行数；values 与 Query::aggregates() 逐项对应。
// COUNT 不用 values；SUM/AVG 保存累计和，MIN/MAX 保存当前极值。
struct GroupState {
    std::uint64_t count = 0; // 该组通过过滤的记录数，也作为 AVG 的分母。
    std::vector<double> values; // 各聚合项的累计和或极值，COUNT 对应槽位不用。
};

// 合并相同查询的组状态；两侧 values 的布局须与 query 一致。
void merge_state(GroupState& target, const GroupState& source, const Query& query);

// Aggregator（聚合器）：按查询过滤记录、累计分组统计，并支持合并与结果输出。
// 每个工作线程独占一张表，线程结束后由主线程合并到最终表。
// query_ 是借用引用，Query 必须比 Aggregator 活得更久。
class Aggregator {
public:
    Aggregator(const Query& query, std::size_t max_groups);
    void consume(const Row& row);
    void merge(const Aggregator& other);
    // 按分组键的无符号字节序写出数据行，返回行数；表头由 engine 写出。
    std::uint64_t write(std::ostream& output) const;
    std::uint64_t matched() const { return matched_; }
private:
    GroupState& find_or_create(const std::string& key, std::uint64_t line);
    const Query& query_; // 借用的只读查询，决定过滤和聚合状态布局。
    std::size_t max_groups_; // 本张表允许创建的最大组数。
    std::unordered_map<std::string, GroupState> groups_; // 原始分组键到统计状态的映射。
    std::uint64_t matched_ = 0; // 本聚合器累计接收的匹配行数，合并时一并累加。
};

} // namespace flash_csv
