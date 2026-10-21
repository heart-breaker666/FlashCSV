#pragma once

#include "flash_csv/types.hpp"
#include <optional>

namespace flash_csv {

// Filter（过滤条件）：保存绑定后的字段下标及比较条件，供逐行匹配使用。
// 编译后的过滤条件：保存字段下标，并提前解析数值比较的右侧常量。
struct Filter {
    std::size_t column; // 过滤列在 Row::fields 中的下标。
    FilterSpec spec; // 原始条件，保留比较类型、运算符及文本常量。
    double number = 0; // 数值过滤的右侧常量；字符串过滤不用此项。
};

// Aggregate（聚合项）：保存聚合函数及绑定后的输入字段下标。
// COUNT 不读取字段，column 仅对其他聚合函数有意义。
struct Aggregate {
    Function function; // 当前聚合项使用的函数。
    std::size_t column; // 输入字段下标；COUNT 使用占位值 0。
};

// Query（查询）：将运行配置绑定到表头，提供过滤、列选择和聚合定义。
// 将 Config 中的列名一次性解析为下标，后续逐行处理无需查找列名。
// 构造完成后只读，可被多个工作线程共享。
class Query {
public:
    Query(const Config& config, const std::vector<std::string>& header);
    // 条件按用户给定顺序执行 AND；遇到不匹配立即返回。
    bool matches(const Row& row) const;
    // 按 --select 顺序复制字段，供导出写入；未选列时保留全部列。
    std::vector<std::string> project(const Row& row) const;
    const std::vector<std::string>& output_header() const { return output_header_; }
    const std::vector<std::string>& input_header() const { return input_header_; }
    const std::vector<Aggregate>& aggregates() const { return aggregates_; }
    std::optional<std::size_t> group_column() const { return group_column_; }
private:
    std::vector<std::string> input_header_; // 输入列名副本，供数值错误定位。
    std::vector<std::string> output_header_; // 按最终输出顺序排列的列名。
    std::vector<std::size_t> select_; // 导出字段下标，顺序与输出列一致。
    std::vector<Filter> filters_; // 已解析列名和常量的有序过滤条件。
    std::vector<Aggregate> aggregates_; // 聚合项，与 GroupState::values 逐项对应。
    std::optional<std::size_t> group_column_; // 分组字段下标；无值表示整体统计。
};

// 严格验证十进制语法、完整转换和有限性；数据错误默认退出码为 1，
// 查询构造时传入 2，将非法数值常量报告为配置错误。
double parse_number(const std::string& value, std::uint64_t line,
                    const std::string& column, int exit_code = 1);
Config parse_arguments(int argc, char* argv[]);
std::string usage();

} // namespace flash_csv
