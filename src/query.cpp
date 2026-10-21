#include "flash_csv/query.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace flash_csv {

std::string sample(const std::string& value) {
    std::string result; // 截断、转义后用于错误诊断的文本。
    const std::size_t end = std::min<std::size_t>(value.size(), 48); // 最多采样的原始字节数。
    for (std::size_t i = 0; i < end; ++i) {
        const auto c = static_cast<unsigned char>(value[i]); // 当前样本字节，按无符号值判断。
        if (c < 32 || c == 127 || c == '\'' || c == '\\') {
            constexpr char hex[] = "0123456789abcdef"; // 将高/低四位映射为十六进制字符。
            result += "\\x";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    if (value.size() > end) result += "...";
    return result;
}

Error::Error(int exit_code, const std::string& kind, const std::string& reason,
             std::uint64_t line, const std::string& column)
    : std::runtime_error(kind + (line ? " line " + std::to_string(line) : "") +
                         (column.empty() ? "" : " column '" + sample(column) + "'") +
                         ": " + reason), exit_code_(exit_code) {}

std::size_t Row::field_bytes() const {
    std::size_t total = 0; // 当前记录的字段内容字节总数。
    for (const auto& field : fields) {
        // 用减法检查剩余空间，避免先相加时已经发生无符号回绕。
        if (field.size() > std::numeric_limits<std::size_t>::max() - total)
            throw Error(1, "capacity", "field byte count overflow", line);
        total += field.size();
    }
    return total;
}

void add_count(std::uint64_t& target, std::uint64_t value, std::uint64_t line) {
    if (value > std::numeric_limits<std::uint64_t>::max() - target)
        throw Error(1, "count_overflow", "counter overflow", line);
    target += value;
}

std::string format_number(double value) {
    char buffer[64]; // 保存格式化后的十进制字符，无需分配临时字符串。
    // max_digits10 保证有限 double 转为十进制后再解析能恢复原值。
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::general,
                                    std::numeric_limits<double>::max_digits10); // result 含写入终点及错误码。
    if (result.ec != std::errc{}) throw Error(1, "number", "number formatting failed");
    return std::string(buffer, result.ptr);
}

double parse_number(const std::string& value, std::uint64_t line,
                    const std::string& column, int exit_code) {
    // 先验证项目允许的十进制语法，排除 from_chars 可能接受的其他形式。
    // 尾数允许负号但不允许正号；指数部分可带正负号，且必须包含数字。
    std::size_t i = 0; // 数值文本中下一个待检查字符的下标。
    if (i < value.size() && value[i] == '-') ++i;
    auto digits = [&] { // 消费连续数字，并返回本段数字的个数。
        const auto start = i; // 本段数字开始的位置，用于计算长度。
        while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
        return i - start;
    };
    const auto integral = digits(); // 小数点前的数字个数。
    std::size_t fractional = 0; // 小数点后的数字个数。
    if (i < value.size() && value[i] == '.') { ++i; fractional = digits(); }
    bool valid = integral + fractional > 0; // 尾数至少有一位数字，之后再校验指数。
    if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
        ++i;
        if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
        valid = digits() > 0 && valid;
    }
    double number = 0; // 接收 from_chars 转换出的数值。
    if (valid && i == value.size()) {
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number,
                                            std::chars_format::general); // result 含解析终点及错误码。
        // 三层检查：语法正确、完整且未越界地转换、结果不是 NaN/Infinity。
        if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
            std::isfinite(number)) return number;
    }
    throw Error(exit_code, "invalid_number", "invalid or out-of-range number '" + sample(value) + "'",
                line, column);
}

static std::string function_name(Function function) {
    switch (function) {
        case Function::Count: return "count";
        case Function::Sum: return "sum";
        case Function::Min: return "min";
        case Function::Max: return "max";
        case Function::Avg: return "avg";
    }
    throw Error(2, "config", "unknown aggregate function");
}

Query::Query(const Config& config, const std::vector<std::string>& header)
    : input_header_(header) {
    // 构造阶段解析列名，并提前检查不存在的列和非法过滤常量。
    // 扫描数据时只访问已保存的下标，免去每行重复查找和解析常量。
    std::unordered_map<std::string, std::size_t> columns; // 表头列名到字段下标的映射。
    for (std::size_t i = 0; i < header.size(); ++i) columns.emplace(header[i], i);
    auto resolve = [&](const std::string& name) { // 查找下标，不存在的列名立即报错。
        const auto found = columns.find(name); // 查找结果；end 表示没有该列。
        if (found == columns.end()) throw Error(2, "config", "unknown column '" + sample(name) + "'");
        return found->second;
    };
    for (const auto& spec : config.filters) {
        Filter filter{resolve(spec.column), spec, 0}; // 正在编译的单个过滤条件。
        if (spec.numeric) filter.number = parse_number(spec.value, 0, spec.column, 2);
        else if (spec.op != Compare::Eq && spec.op != Compare::Ne)
            throw Error(2, "config", "str filters support eq/ne only");
        filters_.push_back(std::move(filter));
    }
    if (config.mode == Mode::Export) {
        if (config.select.empty()) {
            for (std::size_t i = 0; i < header.size(); ++i) select_.push_back(i);
            output_header_ = header;
        } else {
            for (const auto& name : config.select) {
                select_.push_back(resolve(name));
                output_header_.push_back(name);
            }
        }
    } else {
        if (config.aggregates.empty()) throw Error(2, "config", "at least one --agg is required");
        if (!config.group_by.empty()) {
            group_column_ = resolve(config.group_by);
            output_header_.push_back(config.group_by);
        }
        for (const auto& spec : config.aggregates) {
            const bool count = spec.function == Function::Count; // 是否为不读取字段的 COUNT。
            // COUNT 只看行数，使用占位下标 0，无需解析输入列。
            aggregates_.push_back({spec.function, count ? 0 : resolve(spec.column)});
            output_header_.push_back(function_name(spec.function) + (count ? "" : "(" + spec.column + ")"));
        }
    }
    // 投影或聚合均可能产生重名输出列，在创建输出文件前统一拒绝。
    std::unordered_set<std::string> names; // 已生成的输出列名，用于检测重复。
    for (const auto& name : output_header_)
        if (!names.insert(name).second)
            throw Error(2, "config", "duplicate output column '" + sample(name) + "'");
}

bool Query::matches(const Row& row) const {
    // 保持命令行条件顺序：较早条件已排除的记录不会转换后续数值字段。
    for (const auto& filter : filters_) {
        const auto& text = row.fields[filter.column]; // 当前条件读取的原始字段文本。
        bool match = false; // 当前单个过滤条件是否成立。
        if (!filter.spec.numeric) {
            match = (text == filter.spec.value);
            if (filter.spec.op == Compare::Ne) match = !match;
        } else {
            const double value = parse_number(text, row.line, input_header_[filter.column]); // 当前字段的数值。
            const double rhs = filter.number; // 比较右侧的数值常量，已预先解析。
            switch (filter.spec.op) {
                case Compare::Eq: match = value == rhs; break;
                case Compare::Ne: match = value != rhs; break;
                case Compare::Gt: match = value > rhs; break;
                case Compare::Ge: match = value >= rhs; break;
                case Compare::Lt: match = value < rhs; break;
                case Compare::Le: match = value <= rhs; break;
            }
        }
        if (!match) return false;
    }
    return true;
}

std::vector<std::string> Query::project(const Row& row) const {
    std::vector<std::string> result; // 按选择列顺序收集的输出字段。
    result.reserve(select_.size());
    for (auto column : select_) result.push_back(row.fields[column]);
    return result;
}

static std::size_t positive_integer(const std::string& text, const std::string& option) {
    std::size_t value = 0; // 解析出的线程数或容量值。
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value); // 解析终点及错误码。
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
        throw Error(2, "arguments", option + " requires a positive integer");
    return value;
}

Config parse_arguments(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        Config help; // 仅用于返回帮助请求的配置。
        help.help = true;
        return help;
    }
    if (argc < 3) throw Error(2, "arguments", "expected MODE INPUT; use --help");
    Config config; // 随参数扫描逐项填充的执行配置。
    const std::string mode = argv[1]; // 用户指定的模式名称。
    if (mode == "export") config.mode = Mode::Export;
    else if (mode == "aggregate") config.mode = Mode::Aggregate;
    else throw Error(2, "arguments", "unknown mode '" + sample(mode) + "'");
    if (std::string(argv[2]) == "--help") { config.help = true; return config; }
    config.input = argv[2];
    if (config.input.empty())
        throw Error(2, "arguments", "one input path is required");
    // --where、--select、--agg 可重复累积；single 拒绝输出路径、容量等选项重复。
    std::unordered_set<std::string> singles; // 已出现的单值选项名称。
    auto single = [&](const std::string& option) { // 登记单值选项，重复时立即报错。
        if (!singles.insert(option).second) throw Error(2, "arguments", "duplicate " + option);
    };
    auto aggregate_only = [&] { // 验证当前选项是否用于聚合模式。
        if (config.mode != Mode::Aggregate) throw Error(2, "arguments", "option requires aggregate mode");
    };
    int i = 3; // argv 扫描位置，跳过程序名、模式及输入路径。
    // i 始终指向下一项参数；按选项所需数量取值，允许列名等以 -- 开头。
    auto value = [&](const std::string& option) -> std::string { // 取下一项作为当前选项的值。
        if (i == argc) throw Error(2, "arguments", "missing value for " + option);
        const std::string result = argv[i++]; // 本次取出的选项值。
        return result;
    };
    while (i < argc) {
        const std::string option = argv[i++]; // 当前待处理的选项名称。
        if (option == "--help") { config.help = true; return config; }
        else if (option == "--output") { single(option); config.output = value(option); }
        else if (option == "--select") {
            if (config.mode != Mode::Export) throw Error(2, "arguments", "--select requires export mode");
            config.select.push_back(value(option));
        } else if (option == "--where") {
            FilterSpec filter; // 本次 --where 的文字形式条件。
            filter.column = value(option);
            const auto type = value(option); // 比较类型：str 或 num。
            if (type != "num" && type != "str") throw Error(2, "arguments", "filter type must be str or num");
            filter.numeric = type == "num";
            const auto op = value(option); // 命令行中的比较运算符名称。
            // 名称顺序须与 Compare 枚举一致，下方将位置转换为枚举值。
            const std::vector<std::string> names{"eq", "ne", "gt", "ge", "lt", "le"}; // 运算符查找表。
            const auto found = std::find(names.begin(), names.end(), op); // 位置对应 Compare 枚举值。
            if (found == names.end() || (!filter.numeric && found - names.begin() > 1))
                throw Error(2, "arguments", "invalid filter operator");
            filter.op = static_cast<Compare>(found - names.begin());
            // 过滤值可能是字面文本 --help 等，此处直接取值，不按选项解释。
            if (i == argc) throw Error(2, "arguments", "missing filter value");
            filter.value = argv[i++];
            config.filters.push_back(std::move(filter));
        } else if (option == "--group-by") {
            aggregate_only(); single(option); config.group_by = value(option);
            if (config.group_by.empty()) throw Error(2, "arguments", "empty group column");
        } else if (option == "--agg") {
            aggregate_only();
            const auto name = value(option); // 本次 --agg 指定的函数名称。
            AggSpec spec; // 正在填充的单个聚合要求。
            if (name == "count") spec.function = Function::Count;
            else if (name == "sum") spec.function = Function::Sum;
            else if (name == "min") spec.function = Function::Min;
            else if (name == "max") spec.function = Function::Max;
            else if (name == "avg") spec.function = Function::Avg;
            else throw Error(2, "arguments", "unknown aggregate '" + sample(name) + "'");
            if (spec.function != Function::Count) spec.column = value(option);
            config.aggregates.push_back(std::move(spec));
        } else {
            single(option);
            // 所有容量选项共用正整数解析，通过指针写入相应配置成员。
            std::size_t* destination = nullptr; // 指向当前选项要修改的配置成员。
            if (option == "--threads") { aggregate_only(); destination = &config.threads; }
            else if (option == "--max-groups") { aggregate_only(); destination = &config.limits.max_groups; }
            else if (option == "--max-record-bytes") destination = &config.limits.max_record_bytes;
            else if (option == "--batch-bytes") destination = &config.limits.batch_bytes;
            else if (option == "--batch-rows") destination = &config.limits.batch_rows;
            else if (option == "--queue-batches") destination = &config.limits.queue_batches;
            else throw Error(2, "arguments", "unknown option '" + sample(option) + "'");
            *destination = positive_integer(value(option), option);
        }
    }
    if (config.output.empty()) throw Error(2, "arguments", "--output is required");
    if (config.threads > 32) throw Error(2, "arguments", "--threads must be 1..32");
    if (config.limits.batch_bytes < config.limits.max_record_bytes)
        throw Error(2, "arguments", "--batch-bytes must be >= --max-record-bytes");
    if (config.mode == Mode::Aggregate && config.aggregates.empty())
        throw Error(2, "arguments", "at least one --agg is required");
    return config;
}

std::string usage() {
    return "FlashCSV: bounded CSV export and aggregation\n"
           "Usage: flash_csv {export|aggregate} INPUT --output PATH [options]\n"
           "  --select COLUMN                    export; repeat to choose column order\n"
           "  --where COLUMN {str|num} OP VALUE   ordered short-circuit AND\n"
           "    str: eq/ne; num: eq/ne/gt/ge/lt/le\n"
           "  --group-by COLUMN                  aggregate; optional single column\n"
           "  --agg count|{sum|min|max|avg} COLUMN aggregate; repeat (count has no column)\n"
           "  --threads N                        aggregate; default 1, range 1..32\n"
           "  --max-record-bytes N               default 65536 (excludes LF/CRLF)\n"
           "  --batch-bytes N                    default 1048576, >= record limit\n"
           "  --batch-rows N                     default 4096\n"
           "  --queue-batches N                  default 4\n"
           "  --max-groups N                     aggregate; default 100000 per table\n"
           "  --help                             show this help\n"
           "Output directory must exist; output must not exist. All capacities are positive.\n";
}

} // namespace flash_csv
