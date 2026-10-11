# CsvFlow

用 C++17 编写的命令行 CSV 处理引擎，定位于 C++ 学习与简历展示。计划支持流式读取、过滤、列选择、分组聚合和基础多线程，核心代码目标为 1,000–2,500 行。

## 当前状态

开发前准备已建立：设计通过审阅，实施计划、开发约定、交接记录和小型验收数据已整理。核心 C++、CMake 构建和测试执行器尚未实现，下面的引擎命令是目标用法，不代表已有可运行程序。

## 文档入口

| 文件 | 用途 |
|---|---|
| [设计文档](docs/superpowers/specs/2026-10-11-csvflow-design.md) | 功能行为、架构、容量与验收依据 |
| [实施计划](docs/implementation-plan.md) | 任务依赖、接口、测试与完成条件 |
| [开发约定](AGENTS.md) | 项目内稳定的开发规则 |
| [交接记录](docs/HANDOFF.md) | 实际状态、验证证据和下一步 |
| [验收数据说明](tests/fixtures/README.md) | 输入样例、预期结果及案例清单 |

## 开发环境

Linux、支持 C++17 的编译器与标准库、CMake、Make、Python 3.9 或以上。核心实现不依赖第三方库；测试计划使用 CTest、标准库 C++ 测试程序与 Python 命令行测试。

性能阶段使用 `/usr/bin/time -v` 测量峰值 RSS，最终核心代码量通过 `cloc` 统计。当前机器的实际版本与缺失工具见交接记录。

## 目标功能

- 导出：单线程过滤与列选择，保持输入顺序。
- 聚合：单列分组或整体统计，支持 COUNT、SUM、MIN、MAX、AVG。
- 并行：一个读取线程与可配置数量的计算线程，局部聚合后合并。
- 容量：约束记录、批次、队列和分组；超限明确报错。
- 数据规则：需要数值处理的空字段或非法字段报错；不支持字段内换行。

输入文件可以大于内存，但分组结果须适合配置的容量。线程数增加可能增加局部表内存，实际吞吐量和加速比由基准测试确定。

## 目标用法

以下命令在对应实施任务完成后才可使用。输出目录需事先存在，目标文件不能已存在。

```bash
./build/csvflow export tests/fixtures/sales.csv \
  --output /tmp/csvflow-filtered.csv \
  --select city --select amount \
  --where amount num gt 100

./build/csvflow aggregate tests/fixtures/sales.csv \
  --output /tmp/csvflow-stats.csv \
  --group-by city \
  --agg count --agg sum amount --agg avg amount \
  --threads 4
```

第一条的结果为上海、200；第二条的上海 COUNT=2、SUM=300、AVG=150，北京 COUNT=1、SUM=80、AVG=80。聚合输出按分组键的字节顺序排序。

## 构建与验证

Task 1 实现 CMake 与测试目标后使用以下命令；当前尚不能构建项目：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

现在可检查案例清单的 JSON 格式：

```bash
python3 -m json.tool tests/fixtures/cases.json > /dev/null
```

JSON 格式正确仅说明数据清单可读取，不代表引擎通过任何验收测试。
