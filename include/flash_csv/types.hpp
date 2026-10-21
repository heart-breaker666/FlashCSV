#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace flash_csv {

// Error（错误异常）：在模块间传递错误诊断和进程退出码。
// 诊断信息由基类保存，退出码由 main 返回给调用者。
// line=0、column 为空时省略对应定位，适用于尚未读取数据的配置错误。
class Error : public std::runtime_error {
public:
    Error(int exit_code, const std::string& kind, const std::string& reason,
          std::uint64_t line = 0, const std::string& column = {});
    int exit_code() const noexcept { return exit_code_; }
private:
    int exit_code_; // 进程退出码：配置错误为 2，运行错误为 1。
};

// Row（数据行）：保存一条 CSV 记录的原始行号和解码字段。
// 每条记录拥有解码后的字段字符串，不引用 CsvReader 的可复用缓冲区。
struct Row {
    // 原始输入行号包含表头（第 1 行），用于在过滤、聚合时定位错误。
    std::uint64_t line = 0;
    std::vector<std::string> fields; // 按输入列顺序保存的解码字段。
    // 只累计字段内容的字节数，不包含 CSV 分隔符、引号或容器开销。
    std::size_t field_bytes() const;
};

// Batch（数据批次）：将多条记录及其字段内容字节数打包，供队列传递和批量处理。
// 批次移动到队列或工作线程时，记录及其字符串的所有权一起转移。
struct Batch {
    std::vector<Row> rows; // 这一批拥有的记录。
    std::size_t bytes = 0; // 所有记录的字段内容字节数之和。
};

// Limits（容量限制）：集中保存记录、批次、队列和聚合表的容量上限。
// 分别限制单条原始记录、批次内容/行数、排队批次和每张聚合表的组数。
// 这些容量控制工作量；字符串、容器等额外开销仍会占用内存。
struct Limits {
    std::size_t max_record_bytes = 64 * 1024; // 单条原始记录的字节上限，不含 LF/CRLF。
    std::size_t batch_bytes = 1024 * 1024; // 单批解码字段内容的字节上限。
    std::size_t batch_rows = 4096; // 单批记录数上限，也约束空字段记录。
    std::size_t queue_batches = 4; // 等待处理的批次数上限，不含线程已领取的批次。
    std::size_t max_groups = 100000; // 每张局部表及最终合并表的分组数上限。
};

// Mode（执行模式）：选择明细导出或聚合统计。
enum class Mode { Export, Aggregate };
// Compare（比较运算符）：指定过滤条件使用的相等、不等或大小比较。
enum class Compare { Eq, Ne, Gt, Ge, Lt, Le };
// Function（聚合函数）：指定计数、求和、最小值、最大值或平均值计算。
enum class Function { Count, Sum, Min, Max, Avg };

// FilterSpec（过滤条件配置）：保存尚未绑定字段下标的列名、比较方式和常量。
struct FilterSpec {
    std::string column; // 要比较的输入列名。
    bool numeric = false; // true 按数值比较，false 按原始字符串比较。
    Compare op = Compare::Eq; // eq/ne/gt/ge/lt/le 对应的比较运算。
    std::string value; // 命令行给定的比较常量，尚未转换为数字。
};

// AggSpec（聚合配置）：保存要执行的聚合函数及其输入列名。
struct AggSpec {
    Function function = Function::Count; // 要执行的聚合函数。
    std::string column; // 聚合输入列名；COUNT 不使用此项。
};

// Config（运行配置）：汇总输入输出路径、查询条件、线程数和容量限制。
// 命令行解析的结果仍保留列名；读到表头后，由 Query 解析为字段下标。
struct Config {
    bool help = false; // 是否只显示帮助后退出。
    Mode mode = Mode::Export; // 导出明细或计算聚合。
    std::string input; // 输入 CSV 文件路径。
    std::string output; // 最终输出路径，要求尚不存在。
    std::vector<std::string> select; // 导出列及其顺序；为空表示全部列。
    std::vector<FilterSpec> filters; // 按命令行顺序执行的过滤条件。
    std::string group_by; // 分组列名；为空表示整体统计。
    std::vector<AggSpec> aggregates; // 按输出顺序保存的聚合要求。
    std::size_t threads = 1; // 聚合工作线程数，不含主线程。
    Limits limits; // 本次执行使用的容量限制。
};

// 截断并转义诊断样本，避免超长字段或控制字符影响错误信息。
std::string sample(const std::string& value);
// 与区域设置无关的 double 格式化，保留足够精度供再次解析。
std::string format_number(double value);
// 先检查再相加，统一防止行数、字节数等无符号计数器回绕。
void add_count(std::uint64_t& target, std::uint64_t value, std::uint64_t line = 0);

} // namespace flash_csv
