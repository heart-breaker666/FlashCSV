# CsvFlow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现符合已确认设计的 CSV 命令行引擎，完成正确性、并发失败、容量和性能验证，核心 C++ 控制在 1,000–2,500 行。

**Architecture:** 导出由主线程流式读取、过滤、选列并保序写出。聚合由主线程投递拥有数据的批次，计算线程维护局部表，结束后合并并排序写出。队列有界，错误触发取消和线程回收，输出成功后才发布。

**Tech Stack:** Linux、C++17、CMake、标准库；必要的 Linux 临时文件操作；CTest 与轻量 C++ 测试、Python 3.9+ 命令行及基准脚本。

**Spec:** [已审阅设计](superpowers/specs/2026-10-11-csvflow-design.md)。行为与验收标准以设计为准；本文不另定义一份需求。

**Status:** 开发前准备已完成，以下实施任务尚未执行。用户审阅本计划并决定实施方式后进入开发。

## Global Constraints

- 平台为 Linux，语言为 C++17，构建工具为 CMake；核心只使用标准库及必要的 Linux 文件操作。
- 核心 `include/`、`src/` 中 C++ 代码合计 1,000–2,500 行，以 `cloc` 代码行统计；测试、文档、脚本另计。
- 导出单线程且保序；聚合只支持单列分组或整体统计，不引入 SQL、JOIN、多行字段、落盘或多线程导出。
- 默认记录上限 65,536 字节，批次字段内容上限 1,048,576 字节，批次行数 4,096，队列批次数 4，分组上限 100,000。
- 默认 1 个计算线程，允许 1–32；聚合还有一个读取线程，不将工作线程数量冒称整个进程线程数量。
- 数据格式、数值转换和空结果规则完整引用设计第 4 节；容量与并发规则引用第 7、8 节。
- 输出到新文件，通过同目录临时文件与不覆盖目标的发布操作完成；不覆盖输入或已有输出。
- 浮点验收容差为 `abs(a-b) <= 1e-9 * max(1, abs(a), abs(b))`，COUNT、表头和分组集合精确比较。
- 失败须记录实际原因并回收资源；不能以跳过错误记录改变统计结果。

## 执行方式与验证约定

当前授权范围是准备文档与验收数据。后续可由用户自行按任务实现，或授权编码助手按任务执行；本计划不自动启动开发。

每个任务按“具体行为测试 → 确认失败原因 → 最小实现 → 本任务及受影响测试 → 更新交接 → 提交”推进。单条步骤尽量控制为一次操作；涉及多个边界测试时重复这个循环，而不是一次写完整模块。

代码块提供接口和关键算法，其他实现按明确行为与测试展开。所有测试中的 `require` 使用始终有效的检查，不能依赖 Release 下会消失的 `assert`。

以下命令均从项目根目录运行，在 Task 1 建立 CMake 后才可使用：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## 文件职责与任务依赖

| 文件 | 责任 | 首次建立 |
|---|---|---|
| `include/csvflow/types.hpp` | Row、Batch、Limits、RunStats 公共值类型 | Task 1 |
| `include/csvflow/csv.hpp`, `src/csv.cpp` | 受限流式 CSV 读写 | Task 1 |
| `include/csvflow/query.hpp`, `src/query.cpp` | 列编译、过滤、数值转换、查询配置 | Task 2 |
| `include/csvflow/engine.hpp`, `src/engine.cpp` | 文件与输出生命周期、执行组织 | Task 3 |
| `src/main.cpp` | CLI 解析、错误、统计 | Task 3 |
| `include/csvflow/aggregate.hpp`, `src/aggregate.cpp` | 局部分组状态、合并和结果写出 | Task 4 |
| `include/csvflow/pipeline.hpp`, `src/pipeline.cpp` | 有界队列、取消状态、并行聚合调度 | Task 5、6 |
| `tests/test_support.hpp`, `tests/test_*.cpp` | 行为与并发不变量测试 | 各对应任务 |
| `tests/test_cli.py` | 调用二进制并核对验收案例 | Task 3、4、7 |
| `tools/generate_csv.py`, `tools/benchmark.py` | 可重复数据与性能测量 | Task 8 |
| `docs/benchmarks.md` | 实际环境、结果、限制和分析 | Task 8 |

依赖顺序：Task 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8。Task 5 的队列独立测试，Task 6 才把它接入聚合。`types.hpp` 只容纳少量共享值类型；新增 `pipeline.cpp` 为非模板调度实现，仍保持设计的六个职责模块。

## Task 1：受限 CSV 读写（里程碑 1）

**Files:** Create `CMakeLists.txt`, `include/csvflow/types.hpp`, `include/csvflow/csv.hpp`, `src/csv.cpp`, `tests/test_support.hpp`, `tests/test_csv.cpp`。

**Spec:** 第 4.1、6、7、9.2 节。

**Interfaces:** 输入为调用者拥有的 `std::istream/std::ostream`，所有记录和批次拥有字段内容；读取器借用流，调用者保证流生命周期。

```cpp
struct Row { std::vector<std::string> fields; std::size_t line; };
struct Batch { std::vector<Row> rows; std::size_t bytes = 0; };
struct Limits {
    std::size_t max_record_bytes = 65536, batch_bytes = 1048576;
    std::size_t batch_rows = 4096, queue_batches = 4, max_groups = 100000;
};
struct RunStats {
    std::uint64_t rows_read = 0, rows_matched = 0, rows_written = 0;
    std::uintmax_t input_bytes = 0;
    std::size_t workers = 0;
    double seconds = 0;
};
class CsvReader {
public:
    CsvReader(std::istream& input, Limits limits);
    const std::vector<std::string>& header() const;
    std::optional<Batch> next_batch();
};
class CsvWriter {
public:
    explicit CsvWriter(std::ostream& output);
    void write(const std::vector<std::string>& fields);
};
```

- [ ] **1. 建立始终有效的检查函数与第一条 CSV 行为测试；CMake 仅建立核心库和 `test_csv`，注册 CTest 名称 `csv`。** CMake minimum 设为 3.16，设置 CXX_STANDARD=17、CXX_STANDARD_REQUIRED=ON、CXX_EXTENSIONS=OFF；include 路径公开为 include，测试检查始终有效，不使用第三方框架。

```cpp
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::istringstream input("city,amount\n\"North,Zone\",100\n");
CsvReader reader(input, Limits{});
auto batch = reader.next_batch();
require(reader.header()[0] == "city", "header");
require(batch && batch->rows.size() == 1, "one record");
require(batch->rows[0].fields[0] == "North,Zone", "quoted comma");
require(batch->rows[0].line == 2, "original line");
```

- [ ] **2. 运行构建，确认失败是 CSV 接口或实现缺失，而非 CMake 配置错误。** Run `cmake -S . -B build -DBUILD_TESTING=ON`，然后 `cmake --build build -j2`。
- [ ] **3. 实现受限读取与四状态解析：字段开始、普通字段、引号字段、引号结束。每读入内容字节先检查上限；在记录边界识别 LF/CRLF，区分 EOF 与 I/O 错误。** 核心检查采用：

```cpp
if (record_bytes == limits.max_record_bytes)
    throw std::runtime_error("record byte limit at line " + std::to_string(line));
++record_bytes;
// 行结束符及仅文件起始处的 BOM 在这一步之前识别并排除。
```

- [ ] **4. 每增加一种状态转换先增加具体测试：`"a,b"`、`"a""b"`、`"a"x`、未闭合引号、未加引号字段中的引号、字段内换行。** 前两项解析为 `a,b` 和 `a"b`，后三类及多行字段抛出带记录起始行号的错误。
- [ ] **5. 实现批次封装，累计解码后的字段字节及记录数；至多缓存一条待加入记录。检查表头唯一且非空、每行字段数量；补充 BOM、CRLF、无末尾换行、空文件和空记录测试。** 在精确 64 字节和 65 字节的自定义上限样例中分别验证成功和失败；表头也受限。
- [ ] **6. 建立写出转义与往返测试。** 以下为单字段写出的核心算法，完整记录再写逗号和 LF，并检查流状态：

```cpp
bool quoted = field.find_first_of(",\"\r\n") != std::string::npos;
if (quoted) out.put('"');
for (char c : field) {
    if (c == '"') out.put('"');
    out.put(c);
}
if (quoted) out.put('"');
```

- [ ] **7. 运行 `ctest --test-dir build -R '^csv$' --output-on-failure`，确认全部通过；更新交接记录并提交 `feat: add bounded CSV reader and writer`。**

**Done:** 合法格式可往返，非法格式有行号，记录和批次边界可验证，不使用无上限的整文件读取或 getline 后置检查。

## Task 2：查询编译与过滤（里程碑 2 的第一部分）

**Files:** Create `include/csvflow/query.hpp`, `src/query.cpp`, `tests/test_query.cpp`；Modify `CMakeLists.txt`，注册 `query`。

**Spec:** 第 3.4、4.2、6、9.2 节。

**Interfaces:** 使用 Task 1 的 Row/Limits。枚举为 `Mode {Export, Aggregate}`、`FieldType {Str, Num}`、`Compare {Eq, Ne, Gt, Ge, Lt, Le}`、`AggKind {Count, Sum, Min, Max, Avg}`。

```cpp
struct FilterSpec {
    std::string column; FieldType type; Compare op; std::string value;
};
struct AggSpec { AggKind kind; std::string column; }; // Count 的 column 为空
struct QuerySpec {
    Mode mode;
    std::filesystem::path input, output;
    std::vector<std::string> select;
    std::vector<FilterSpec> where;
    std::optional<std::string> group;
    std::vector<AggSpec> aggs;
    Limits limits;
    std::size_t threads = 1;
};
struct CompiledFilter {
    std::size_t column; FieldType type; Compare op;
    std::string text; double number = 0;
};
struct CompiledAggregate { AggKind kind; std::optional<std::size_t> column; };
struct CompiledQuery {
    Mode mode;
    std::vector<std::string> input_header, output_header;
    std::vector<std::size_t> selected;
    std::vector<CompiledFilter> where;
    std::optional<std::size_t> group;
    std::vector<CompiledAggregate> aggs;
};
CompiledQuery compile(const std::vector<std::string>& header, const QuerySpec& spec);
bool matches(const Row& row, const CompiledQuery& query);
double parse_number(std::string_view value, std::size_t line, std::string_view column);
```

- [ ] **1. 写明确的数值测试：`.5→0.5`、`1.→1`、`-1.5→-1.5`、`2e3→2000`。对 `""`、`+1`、` 1`、`1x`、`0x10`、NaN、Infinity、`1e9999`、`1e-9999` 验证异常并包含行号/列名。** 样例：

```cpp
require(parse_number(".5", 2, "amount") == 0.5, "decimal fraction");
bool failed = false;
try { (void)parse_number("1x", 3, "amount"); }
catch (const std::exception& e) {
    failed = std::string(e.what()).find("amount") != std::string::npos;
}
require(failed, "invalid number names its column");
```

- [ ] **2. Run `cmake --build build -j2`，确认新测试因接口未实现失败。**
- [ ] **3. 按设计数值语法扫描字符，再使用浮点 `std::from_chars`；拒绝不完整转换、范围错误和非有限值。** 扫描负责禁止正号和十六进制，不以 from_chars 的默认接受范围替代设计语法。

```cpp
double number = 0;
auto result = std::from_chars(value.data(), value.data() + value.size(), number);
if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
    || !std::isfinite(number))
    throw std::runtime_error("invalid number in " + std::string(column)
                             + " at line " + std::to_string(line));
return number;
```

- [ ] **4. 增加列编译测试，再建立一次性列名到下标映射。** 验证选择顺序、未知列、重复选择、重复聚合、输出名冲突及非法 str 运算符；命令行比较值在编译或参数阶段解析。
- [ ] **5. 增加短路测试，再实现按参数顺序比较。** 在 `short_circuit.csv` 上，`city str eq 上海` 在前时跳过北京非法金额；反向排序时报告第 3 行错误。核心控制流：

```cpp
auto compare = [](const auto& a, const auto& b, Compare op) {
    switch (op) {
    case Compare::Eq: return a == b;
    case Compare::Ne: return a != b;
    case Compare::Gt: return a > b;
    case Compare::Ge: return a >= b;
    case Compare::Lt: return a < b;
    case Compare::Le: return a <= b;
    }
    throw std::invalid_argument("unknown comparison");
};
for (const auto& condition : query.where) {
    const auto& field = row.fields.at(condition.column);
    bool accepted = condition.type == FieldType::Str
        ? compare(field, condition.text, condition.op)
        : compare(parse_number(field, row.line,
                               query.input_header.at(condition.column)),
                  condition.number, condition.op);
    if (!accepted) return false;
}
return true;
```

- [ ] **6. Run `ctest --test-dir build -R '^(csv|query)$' --output-on-failure`；更新交接并提交 `feat: compile CSV queries and evaluate filters`。**

**Done:** 查询配置与数据类型规则可单独测试，行循环不再查列名，短路行为与严格数值规则一致。

## Task 3：命令行导出与输出生命周期（里程碑 2 完成）

**Files:** Create `include/csvflow/engine.hpp`, `src/engine.cpp`, `src/main.cpp`, `tests/test_cli.py`, `tests/test_output.cpp`；Modify `CMakeLists.txt`, `README.md`。

**Spec:** 第 3、5、8.3、8.4、9.2 节。

**Interfaces:** 消费 QuerySpec/CompiledQuery、CsvReader/CsvWriter。提供以下接口；`OutputFile` 放在 engine.hpp 的 `csvflow::detail` 中供输出生命周期测试访问，实现位于 engine.cpp，其他业务模块不依赖它。

```cpp
class Engine { public: RunStats run(const QuerySpec& spec); };
QuerySpec parse_args(int argc, char** argv); // 在 main.cpp 中实现
class OutputFile {
public:
    explicit OutputFile(const std::filesystem::path& target);
    std::ostream& stream();
    void publish();
    ~OutputFile();
};
```

- [ ] **1. 从案例清单读取 F01/F02/F03/F09/F10/F11/F12/F14/F15–F20/F23–F25，建立 `export.cli` 测试。先验证二进制缺失/功能缺失的失败，不把此状态标为数据测试通过。** Python 调用骨架：

```python
with tempfile.TemporaryDirectory() as directory:
    output = pathlib.Path(directory) / "result.csv"
    command = [binary, case["mode"], str(fixtures / case["input"]),
               "--output", str(output), *case["args"]]
    result = subprocess.run(command, text=True, encoding="utf-8",
                            capture_output=True, timeout=10)
    assert result.returncode == case["expected"]["exit_code"]
```

- [ ] **2. 实现参数解析：未知参数、缺值、模式冲突、重复单值参数和非法正整数退出 2；`--help` 退出 0。** 在所有相关数值参数上检查完整解析与溢出，线程数限制 1–32，批次字节不得小于记录字节。参数和列相关的配置错误抛出 `std::invalid_argument`，main 映射为 2；格式、容量及 I/O 错误用 `std::runtime_error`，映射为 1。设计第 8.4 节的参数/运行错误分类据此落实，不新增错误恢复模式。
- [ ] **3. 实现导出主循环，并验证输出表头与行序。**

```cpp
CsvWriter writer(output.stream());
writer.write(query.output_header);
while (auto batch = reader.next_batch()) {
    for (const Row& row : batch->rows) {
        ++stats.rows_read;
        if (!matches(row, query)) continue;
        ++stats.rows_matched;
        std::vector<std::string> selected;
        for (auto index : query.selected) selected.push_back(row.fields[index]);
        writer.write(selected);
        ++stats.rows_written;
    }
}
output.publish();
```

- [ ] **4. 实现 OutputFile：同目录 mkstemp 创建唯一临时路径；管理文件关闭和清理；完成后通过 `create_hard_link` 发布，再删除临时路径。** 写入/flush/close 均检查失败，析构不能抛异常，清理错误不能替代原始错误。
- [ ] **5. 加入已有目标、输入等于输出、目录不存在、写出故障及发布竞争的行为测试，CTest 名称 `output`。** 已有目标内容前后保持一致；最终发布前模拟创建目标，发布必须失败。用自定义失败 streambuf 测 CsvWriter 写出故障；OutputFile 的 flush/close 故障通过内部文件操作封装注入，不增加生产 CLI 参数，测试不依赖 `/dev/full` 和执行用户权限。
- [ ] **6. 实现 RunStats 与 stderr 报告，耗时使用 steady_clock。** 仅成功时打印完整吞吐报告，输入字节用文件实际大小。导出 `workers=0`，有数据错误退出 1，stderr 包含原始行号和适用的列名。
- [ ] **7. Run `ctest --test-dir build -R '^(csv|query|output|export.cli)$' --output-on-failure`；核对 README 导出示例，更新交接，提交 `feat: add ordered CSV export CLI`。**

**Done:** 可运行的导出命令，明确的退出码与统计，失败不留下最终结果，不覆盖已有文件。

## Task 4：聚合状态、合并和单工作线程统计（里程碑 3）

**Files:** Create `include/csvflow/aggregate.hpp`, `src/aggregate.cpp`, `tests/test_aggregate.cpp`；Modify `src/engine.cpp`, `src/main.cpp`, `tests/test_cli.py`, `CMakeLists.txt`, `README.md`。

**Spec:** 第 3.2、3.3、4.2、4.3、6、7、9.2、9.3 节。

**Interfaces:** 聚合器消费已经通过过滤的 Row，借用在整个运行期间有效的 CompiledQuery；内部拥有分组键。

```cpp
struct NumericState { double sum = 0, min = 0, max = 0; bool seen = false; };
struct GroupState { std::uint64_t count = 0; std::vector<NumericState> values; };
class Aggregator {
public:
    Aggregator(const CompiledQuery& query, std::size_t max_groups);
    void consume(const Row& row);
    void merge(Aggregator&& other);
    std::uint64_t write_result(CsvWriter& writer) const; // 写数据行，返回行数
    std::size_t group_count() const;
};
```

- [ ] **1. 建立 `aggregate` 测试，使用 sales.csv 的已知五种结果，以及 header-only 整体/分组结果；构建确认聚合接口缺失。** 合并测试直接构造两个聚合器：第一份上海 100，第二份上海 200、北京 80。

```cpp
QuerySpec spec{};
spec.mode = Mode::Aggregate;
spec.group = "city";
spec.aggs = {{AggKind::Count, ""}, {AggKind::Sum, "amount"}};
auto query = compile({"city", "amount"}, spec);
Aggregator result(query, 10), other(query, 10);
result.consume(Row{{"上海", "100"}, 2});
other.consume(Row{{"上海", "200"}, 3});
result.merge(std::move(other));
std::ostringstream output;
CsvWriter writer(output);
writer.write(query.output_header);
require(result.write_result(writer) == 1, "one group");
std::istringstream input(output.str());
CsvReader reader(input, Limits{});
auto batch = reader.next_batch();
require(batch->rows[0].fields[1] == "2", "merged count");
require(parse_number(batch->rows[0].fields[2], 2, "sum(amount)") == 300,
        "merged sum");
```
- [ ] **2. 实现 consume，每组维护总行数，每个聚合项有一个 NumericState；COUNT 的对应槽不解析字段。** 各项按其函数更新必要状态，检查计数上溢、sum 非有限和插入新组前的上限。

```cpp
if (state.count == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("count overflow");
++state.count;
// 对 SUM/AVG 的槽：
double next = value_state.sum + value;
if (!std::isfinite(next)) throw std::runtime_error("sum overflow");
value_state.sum = next;
```

- [ ] **3. 实现 merge：COUNT 相加，SUM 相加，MIN/MAX 比较；AVG 由合并后的 sum/count 计算。** 对两个不同键的局部表各设上限 1，合并时验证报错，证明全局上限不依赖调度。两份局部 AVG 分别来自 1 行与 3 行时，合并应得到总和除以 4，而不是两个 AVG 的平均。
- [ ] **4. 实现 write_result：收集对稳定分组表的引用，按无符号字节键顺序排序，一次生成并写出一行；不复制全部键和结果。** 使用 `max_digits10` 格式化 double。整体无数据合成 COUNT=0、SUM=0、其余空字段；分组无数据不输出数据行。
- [ ] **5. engine 暂时直接在主线程更新一个聚合器，完成聚合 CLI 的行为验证。** 本阶段尚未接入设计的读取/计算流水线，不发布其性能结论；Task 6 替换执行组织，保留同一聚合逻辑。
- [ ] **6. 将 F04–F08、F13、F21、F22、F26–F28 加入 `aggregate.cli`，对指定数值列使用设计容差；其余字段精确比较。** 浮点字段为空时要求双方均为空，不能调用 float 转换。增加负数、空分组、多数值列和累计溢出的单元测试。
- [ ] **7. Run `ctest --test-dir build --output-on-failure`；更新 README 的聚合可用状态和交接，提交 `feat: add bounded grouping and aggregation`。**

**Done:** 五种聚合及合并正确，两个阶段的分组检查都可验证，整体空数据行为正确。

## Task 5：有界队列、正常关闭与取消（里程碑 4 的第一部分）

**Files:** Create `include/csvflow/pipeline.hpp`, `tests/test_pipeline.cpp`；Modify `CMakeLists.txt`，链接 `Threads::Threads` 并注册 `pipeline`，设置 CTest 超时 10 秒。

**Spec:** 第 8.1、8.2、9.3 节。

**Interfaces:** 模板队列按值接收，再移动入队；正常关闭可排空，取消丢弃等待批次。

```cpp
template<class T> class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity);
    bool push(T value);          // 关闭或取消时返回 false
    std::optional<T> pop();      // 排空关闭或取消时返回 nullopt
    void close();
    void cancel();
};
class FailureState {
public:
    void capture(std::exception_ptr error); // 同步保存首个异常
    void rethrow_if_set();
};
```

- [ ] **1. 写容量 1、移动所有权、正常关闭后排空的测试；构建确认实现缺失。** 已入队一个批次后 close，第一次 pop 仍取得它，第二次 pop 返回空。

```cpp
BoundedQueue<Batch> queue(1);
Batch batch;
batch.rows.push_back(Row{{"A", "1"}, 2});
require(queue.push(std::move(batch)), "enqueue");
queue.close();
auto received = queue.pop();
require(received && received->rows[0].fields[0] == "A", "drain after close");
require(!queue.pop(), "closed empty queue");
require(!queue.push(Batch{}), "no pushes after close");
```
- [ ] **2. 实现 mutex、condition_variable 和状态枚举。** 等待谓词必须包含状态变化：

```cpp
not_full.wait(lock, [&] { return state != Open || queue.size() < capacity; });
if (state != Open) return false;
queue.push_back(std::move(value));
not_empty.notify_one();
```

- [ ] **3. 为空队列消费者等待和满队列生产者等待建立取消测试，再实现 cancel 同时 notify_all 两个条件变量。** 通过 promise/future 或测试专用同步钩子安排阻塞，不把固定 sleep 当作同步证明。
- [ ] **4. 实现 FailureState，并测试多个捕获请求只保留首个实际保存的 exception_ptr；验证取消重复调用安全。**
- [ ] **5. Run `ctest --test-dir build -R '^pipeline$' --output-on-failure`，重复运行并发测试 20 次；更新交接并提交 `feat: add cancellable bounded batch queue`。**

**Done:** 正常关闭和取消语义不同，满/空队列上的等待均可结束，移动数据不引用失效缓冲。

## Task 6：并行聚合与失败回收（里程碑 4 完成）

**Files:** Create `src/pipeline.cpp`, `tests/test_parallel.cpp`；Modify `include/csvflow/pipeline.hpp`, `src/engine.cpp`, `CMakeLists.txt`，注册 `parallel`，超时 10 秒。

**Spec:** 第 5–8、9.3 节。

**Interfaces:** 消费 CsvReader、CompiledQuery、Limits、Aggregator；产生局部状态与计数。CompiledQuery 在结果合并前保持有效。

```cpp
struct PipelineResult {
    std::vector<Aggregator> partials;
    std::uint64_t rows_read = 0, rows_matched = 0;
};
PipelineResult run_pipeline(CsvReader& reader, const CompiledQuery& query,
                            const Limits& limits, std::size_t workers);
```

- [ ] **1. 写相同 sales 数据的 1/2/4 工作线程测试，比较分组集合与 COUNT 精确一致、浮点结果满足容差；加入极小队列配置。构建确认 run_pipeline 缺失。**

```cpp
for (std::size_t workers : {1u, 2u, 4u}) {
    QuerySpec spec{};
    spec.mode = Mode::Aggregate;
    spec.group = "city";
    spec.aggs = {{AggKind::Count, ""}};
    auto query = compile({"city", "amount"}, spec);
    Limits limits;
    limits.queue_batches = 1;
    limits.batch_rows = 1;
    std::istringstream input("city,amount\n上海,100\n北京,80\n上海,200\n");
    CsvReader reader(input, limits);
    auto result = run_pipeline(reader, query, limits, workers);
    require(result.rows_read == 3 && result.rows_matched == 3, "parallel rows");
    Aggregator merged(query, limits.max_groups);
    for (auto& partial : result.partials) merged.merge(std::move(partial));
    std::ostringstream output;
    CsvWriter writer(output);
    require(merged.write_result(writer) == 2, "parallel group count");
    require(output.str() == "上海,2\n北京,1\n", "parallel exact counts");
}
```
- [ ] **2. 固定分配 W 个局部聚合器和局部匹配计数，再启动线程，防止线程运行时 vector 扩容。** 每个线程只访问自己的槽位和只读 query。
- [ ] **3. 实现工作线程循环和异常捕获。**

```cpp
try {
    while (auto batch = queue.pop()) {
        for (const Row& row : batch->rows) {
            if (!matches(row, query)) continue;
            partials[index].consume(row);
            ++matched[index];
        }
    }
} catch (...) {
    failure.capture(std::current_exception());
    queue.cancel();
}
```

- [ ] **4. 实现主线程受限读取与投递，EOF 时正常关闭；投递返回 false 时停止读取并回收线程。** 读取、分配或线程启动异常也捕获到 FailureState 并取消。线程启动循环外建立回收守卫，保证只成功启动部分线程时仍 join。
- [ ] **5. 全部 join 后再检查并重抛错误，再合计线程计数；成功时逐个合并 partials 并释放已合并状态。** engine 使用此流程替换 Task 4 的串行组织，设置 RunStats.workers。
- [ ] **6. 建立读入异常、消费者数值错误、满队列取消、空队列取消、部分线程启动失败的确定性测试。** 用失败 streambuf 和内部测试用线程启动封装注入故障，不增加生产 CLI 参数；每例要求有限时间内返回且全部已启动线程回收。
- [ ] **7. Run `ctest --test-dir build --output-on-failure`；重复 `parallel` 20 次。必要时用 ThreadSanitizer 验证共享状态，记录当前平台是否支持；更新交接并提交 `feat: execute parallel CSV aggregation with cancellation`。**

**Done:** 1 个读取线程加 W 个计算线程的设计落地；任何阶段失败都能结束；性能报告可准确区分线程数量。

## Task 7：完整验收与容量/I/O 故障（里程碑 5 的第一部分）

**Files:** Modify `tests/test_cli.py`, `tests/test_csv.cpp`, `tests/test_query.cpp`, `tests/test_aggregate.cpp`, `tests/test_output.cpp`, `tests/test_parallel.cpp`, `CMakeLists.txt`, `README.md`。

**Spec:** 第 9.2、9.3 节及此前任务的完整边界。

**Interfaces:** 不改变核心模块接口。复用 cases.json 的 28 个案例以及各模块故障测试；CLI 测试可接收 `--suite export/aggregate/all`，CTest 增加 `acceptance.cli`。

- [ ] **1. 让 cases.json 中每个案例都实际运行，并核对 UTF-8、退出码、表头、行序、空字段与错误行号。** 失败时要求输出不存在，检查临时目录内没有本次残留临时文件。

```python
expected = case["expected"]
if expected["exit_code"] == 0:
    with output.open(encoding="utf-8", newline="") as stream:
        actual = list(csv.reader(stream, strict=True))
    assert actual[0] == expected["header"]
    assert len(actual[1:]) == len(expected["rows"])
    for got, want in zip(actual[1:], expected["rows"]):
        assert len(got) == len(want)
        for index, (a, b) in enumerate(zip(got, want)):
            if index in expected["numeric_columns"] and a != "" and b != "":
                av, bv = float(a), float(b)
                assert math.isfinite(av) and math.isfinite(bv)
                assert abs(av - bv) <= 1e-9 * max(1, abs(av), abs(bv))
            else:
                assert a == b
else:
    assert not output.exists()
    if "error_line" in expected:
        assert re.search(r"(?:line|行)\s*" + str(expected["error_line"])
                         + r"(?!\d)", result.stderr)
```

错误分类通过错误内容的原因验证；行号表达式与实际错误报告格式统一，不以整个 stderr 的任意同号数字代替定位。
- [ ] **2. 补充正整数溢出、threads=0/33、batch-bytes 小于 max-record-bytes、未知列、列名冲突、重复参数测试。** 参数类错误为 2，格式与容量类为 1。
- [ ] **3. 补充批次字段字节与行数边界、表头大小边界、输入在读取中失败、flush/close 失败和发布目标竞争测试。** 先验证故障测试失败，再只修复暴露的问题；本步骤不授权无关重构。
- [ ] **4. 对 F04–F08/F21/F22/F26–F28 追加线程 1/2/4 的运行矩阵。** F21 只断言超限失败，不要求错误来自局部表或全局表；Task 4 的直接合并测试负责证明合并上限检查。
- [ ] **5. 验证失败运行的 stderr 不包含成功吞吐量报告，成功运行 CSV 内容不混入统计；核对 README 示例真实可执行。**
- [ ] **6. Run Debug 完整测试，再建立 Release 并运行完整测试，验证检查函数不会因 NDEBUG 失效。**

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure
```

- [ ] **7. 更新交接中的实际通过清单、未验证项和命令；提交 `test: cover CSV engine acceptance and failure cases`。**

**Done:** 28 个初始案例及设计中的并发、容量、I/O 边界有实际验证；未执行的环境相关检查明确列出。

## Task 8：可重复基准、内存与项目展示（里程碑 5 完成）

**Files:** Create `tools/generate_csv.py`, `tools/benchmark.py`, `docs/benchmarks.md`, `tests/test_tools.py`；Modify `CMakeLists.txt`, `README.md`, `docs/HANDOFF.md`。

**Spec:** 第 9.4、10、11 节。

**Interfaces:** 生成脚本参数为 `--output/--rows/--groups/--padding-bytes/--seed`，并在输入文件同目录写出 `OUTPUT.reference.json`，包含生成参数和按组的整数参考状态。benchmark 参数为 `--binary/--input/--threads/--repeat/--report`，其中 threads 接受一个或多个整数，如 `--threads 1 2 4`；组编号从 0 开始，固定种子生成正整数金额，参考结果由整数累计后计算 AVG。benchmark 使用参考侧文件；缺失时明确报错，不把未经核对的运行记录成成功基准。

- [ ] **1. 在临时目录生成 10 行、2 组的小文件，写脚本测试要求相同 seed 得到相同字节、行数准确、生成结果可被引擎聚合并核对。** 运行 `python3 tests/test_tools.py`，确认脚本缺失导致失败。

```python
with tempfile.TemporaryDirectory() as directory:
    a = pathlib.Path(directory) / "a.csv"
    b = pathlib.Path(directory) / "b.csv"
    options = ["--rows", "10", "--groups", "2", "--padding-bytes", "8",
               "--seed", "7"]
    for path in (a, b):
        subprocess.run([sys.executable, str(generator), "--output", str(path),
                        *options], check=True, timeout=10)
    assert a.read_bytes() == b.read_bytes()
    with a.open(encoding="utf-8", newline="") as stream:
        assert len(list(csv.reader(stream))) == 11
```
- [ ] **2. 实现流式生成，不在 Python 内存中保存全部行。** 生成时同步维护受控组数的整数参考状态，参考计算不计入引擎耗时。

```python
rng = random.Random(seed)
writer.writerow(["group", "amount", "padding"])
for index in range(rows):
    group = index % groups
    amount = rng.randrange(1, 10001)
    writer.writerow([f"g{group:08d}", amount, "x" * padding_bytes])
```

- [ ] **3. 实现每配置至少三次基准，结果和统计写入 `/reports/`，生成数据写入 `/data/`；两者不提交 Git。** 每次使用独立的新输出文件，通过 `/usr/bin/time -v` 获取峰值 RSS，保存原始 stderr、命令与完整输入参数；校验结果后才将运行纳入成功样本。
- [ ] **4. 运行固定小分组规模下的百万行、千万行实验，再比较同文件的 1/2/4 个计算线程；记录中位耗时、MiB/s、RSS 和实际 CPU/存储/缓存条件。** 不自动清空系统缓存，不把缓存条件不同的实验归为并行加速。
- [ ] **5. 增加分组数验证内存增长与超限行为。** 大于可用内存的数据实验须先确认磁盘空间和测试条件；未执行则在报告明确列出已测规模及原因，不宣称已实测超内存数据。
- [ ] **6. 用 `cloc include src --by-file` 完成核心代码量统计，合计 C++ 源文件与头文件，不能漏算 hpp；若偏离 1,000–2,500 行，分析重复与功能范围，不压缩格式凑行数。** 当前缺少 cloc，实施本步前补齐工具或明确保留该项未验收。
- [ ] **7. Run `ctest --test-dir build-release --output-on-failure`，生成与基准脚本的小规模验证纳入 CTest `tools`；写入真实结果与瓶颈分析，更新 README 和交接，提交 `perf: add reproducible CSV engine benchmarks`。**

**Done:** 正确性先于性能，报告可复现，明确输入/线程/分组/缓存条件，代码量和已实测规模有证据。

## 设计覆盖索引

| 设计要求 | 实施任务 | 主要验证 |
|---|---|---|
| 第 3 节 CLI 与参数 | 2、3、4、7 | 查询单元测试、CLI 成功/失败 |
| 第 4.1 节 CSV 语法 | 1、7 | csv、F03/F15–F20/F23–F25 |
| 第 4.2 节数值与短路 | 2、4、7 | query、F06/F11–F14/F22 |
| 第 4.3 节空结果、排序 | 4、7 | aggregate、F04–F08/F27/F28 |
| 第 5、6 节架构与所有权 | 1–6 | 模块接口、移动批次、并行结果 |
| 第 7 节容量 | 1、4、5、7 | 精确记录/批次边界、F21/F26、直接合并超限 |
| 第 8.1、8.2 节关闭与取消 | 5、6、7 | 等待状态取消、部分启动失败、超时检查 |
| 第 8.3 节输出 | 3、7 | 已有目标、发布竞争、写入/关闭失败 |
| 第 8.4 节统计与退出码 | 3、7 | stderr 与输出分离、正确退出码 |
| 第 9.4 节性能与内存 | 8 | Release 基准、整数参考结果、RSS |
| 第 10、11 节规模与展示 | 8 | cloc、README、基准报告 |

## 开始实施前的检查

- [x] 用户审阅通过设计文档。
- [x] 开发前准备、任务依赖、验收数据和当前状态已建立。
- [ ] 用户审阅实施计划并确定实施方式。
- [ ] 开始 Task 1；此前不将任何实施任务标为完成。
