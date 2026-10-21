# FlashCSV

基于 **C++17 / Linux** 的命令行 CSV 处理引擎，支持条件筛选、列选择、分组聚合与多线程计算。通过流式读取和有界批次队列处理大文件，无需将整个输入文件载入内存。

## 项目特性

- 自主实现 CSV 解析与转义输出，支持引号、内嵌逗号、BOM、LF / CRLF，并提供数据错误定位。
- 支持字符串和数值过滤、列选择，以及 `COUNT / SUM / MIN / MAX / AVG` 五种聚合函数。
- 采用生产者—消费者模型，各计算线程独占局部哈希聚合表，结束后合并和排序输出。
- 通过记录、批次、队列和分组容量约束资源使用，支持超过机器内存容量的输入文件。
- 提供异常传播、队列取消、线程回收与文件资源管理；结果经临时文件发布，拒绝覆盖已有文件。

```text
导出：读取 → 过滤 → 选列 → 写出
聚合：主线程读取 → 有界批次队列 → 多线程局部聚合 → 合并 → 排序写出
```

支持范围、数据格式、容量规则和错误行为以[设计与验收](docs/design.md)为准。

## 构建与测试

依赖：Linux、支持 C++17 的 GCC / Clang、CMake 3.16+。测试与数据生成脚本需要 Python 3.9+；性能测量还需要 GNU time（`/usr/bin/time`）、`lscpu` 和 `df`。核心程序不依赖第三方 C++ 库。

```bash
git clone https://github.com/heart-breaker666/FlashCSV.git
cd FlashCSV

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release -j2
ctest --test-dir build/release --output-on-failure
```

可执行文件为 `build/release/flash_csv`。只构建程序时，将 `BUILD_TESTING` 设为 `OFF`，此时不需要 Python。调试构建：

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
```

测试包含 `core`（解析、查询、聚合与队列）、`engine`（文件及线程生命周期）和 `acceptance`（CLI、数据边界、并发及 I/O 故障）三组；并发与故障检查设有超时。

## 使用方法

以下命令均在仓库根目录执行。先创建输出目录并查看帮助；每条命令的目标文件须不存在，重复运行时请使用新的输出路径。

```bash
mkdir -p reports
./build/release/flash_csv --help
```

仓库自带示例 `tests/fixtures/sales.csv`：

```csv
city,amount
上海,100
北京,80
上海,200
```

### 过滤与选列

选择 `city`、`amount` 两列，导出金额大于 100 的记录：

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output reports/filtered.csv \
  --select city --select amount \
  --where amount num gt 100
```

输出文件：

```csv
city,amount
上海,200
```

### 分组聚合

按城市统计记录数、总额、最小值、最大值和平均值，使用两个计算线程：

```bash
./build/release/flash_csv aggregate tests/fixtures/sales.csv \
  --output reports/stats.csv \
  --group-by city \
  --agg count --agg sum amount --agg min amount \
  --agg max amount --agg avg amount \
  --threads 2
```

输出文件按分组键排序：

```csv
city,count,sum(amount),min(amount),max(amount),avg(amount)
上海,2,300,100,200,150
北京,1,80,80,80,80
```

### 整体统计与组合过滤

省略 `--group-by` 可进行整体统计；重复 `--where` 可组合过滤条件：

```bash
./build/release/flash_csv aggregate tests/fixtures/sales.csv \
  --output reports/total.csv \
  --where city str eq 上海 --where amount num ge 100 \
  --agg count --agg sum amount --agg avg amount
```

CSV 结果写入 `--output` 指定的文件，成功统计写入 stderr，包括读取、匹配和输出行数、计算线程数、耗时、行/s 与 MiB/s。参数和容量选项见 `--help`，完整使用练习见[从零学习指南](docs/learning-guide.md)。

## 性能测试数据

以下为 **2026-10-12 的已有实测基准**，本次发布保留这些历史测量，并验证当前代码的构建、测试及使用示例。完整的 16 个配置、48 次测量和环境记录见[基准存档](reports/baseline-20261012/summary.md)。

### 测量条件

- Linux / VMware；Intel Core i5-14500HX，虚拟机可见 8 个逻辑 CPU，总内存 **3.54 GiB**；ext4 文件系统。
- GCC 11.5.0，C++17，Release / `-O3 -DNDEBUG`；固定随机种子 `20261012`。
- 每个配置运行 **3 次**，逐次校验分组键、COUNT 和浮点聚合结果；耗时和吞吐取中位数。
- 同时执行五种聚合函数。耗时包括打开输入、读取、计算、合并、排序和结果发布；不包括生成数据及结果校验。
- `--threads` 表示计算线程数，主线程另行读取。峰值 RSS 由 GNU time 测量，下表取三次运行中的最大值。
- 未清理系统文件缓存、未固定 CPU、未隔离其他进程；结果不代表冷盘性能。RSS 不含进程之外的系统文件缓存。

### 规模与线程对比

| 输入行数 | 分组数 | 输入大小 | 计算线程 | 中位耗时 | 吞吐 MiB/s | 最大峰值 RSS MiB |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000,000 | 1,000 | 21.72 MiB | 1 | 0.815 s | 26.67 | 8.39 |
| 1,000,000 | 1,000 | 21.72 MiB | 2 | 0.471 s | 46.13 | 9.62 |
| 1,000,000 | 1,000 | 21.72 MiB | 4 | 0.508 s | 42.75 | 8.09 |
| 10,000,000 | 1,000 | 226.78 MiB | 1 | 8.070 s | 28.10 | 8.52 |
| 10,000,000 | 1,000 | 226.78 MiB | 2 | 4.606 s | 49.24 | 9.39 |
| 10,000,000 | 1,000 | 226.78 MiB | 4 | 5.006 s | 45.30 | 10.50 |
| 1,000,000 | 100,000 | 21.72 MiB | 1 | 1.309 s | 16.60 | 34.89 |
| 1,000,000 | 100,000 | 21.72 MiB | 4 | 0.792 s | 27.43 | 77.18 |
| 10,000,000 | 1,000 | 4.99 GiB | 1 | 32.492 s | 157.26 | 8.46 |

在千万行、1,000 分组配置下，两个计算线程约为单计算线程的 **1.75 倍**，处理速度约 **217 万行/秒**。该配置下四个计算线程未继续加速；高分组数还会增加局部表与合并成本。

4.99 GiB 测试为每行增加 512 字节填充字段，输入大于虚拟机总内存；运行时另设 **256 MiB 地址空间上限**，三次运行的最大峰值 RSS 为 **8.46 MiB**。该低 RSS 结果对应 1,000 个分组，不能推广为任意分组数下的固定内存占用；较宽记录的字节吞吐也不等于更高的行处理速度。

### 复现基准

先完成上面的 Release 构建，再执行：

```bash
# 可选：单独生成百万行、1,000 分组的数据及独立预期结果
python3 tools/generate_data.py --rows 1000000 --groups 1000

# 常规矩阵：十万、百万、千万行，比较线程数和分组数
python3 tools/benchmark.py

# 增加大于机器内存的输入；脚本会检查磁盘空间
python3 tools/benchmark.py --large
```

基准脚本自动生成或复用输入，每配置至少运行三次并逐次校验结果。数据保存在 `data/`，新测量的环境、原始记录和汇总保存在 `reports/benchmark-*/`。`--large` 按本机内存调整填充长度，其他机器上的输入大小可能不同。

构建产物、生成数据和新产生的完整运行目录由 `.gitignore` 排除；仓库保留小型历史基准存档，供核对本节数据。

## 目录与文档

```text
include/flash_csv/   公共类型与模块接口
src/                CSV、查询、聚合、队列、执行引擎与 CLI
tests/              核心测试、引擎测试、CLI 验收与小型输入
tools/              数据生成和性能测量脚本
docs/               设计、使用练习与代码讲解
reports/baseline-20261012/  已有实测数据与环境存档
```

- [设计与验收](docs/design.md)：功能范围、数据与容量规则、错误行为。
- [从零学习指南](docs/learning-guide.md)：完整使用练习和预期结果。
- [代码讲解](docs/code-walkthrough.md)：调用顺序、所有权、同步与异常处理。
- [验收数据](tests/fixtures/README.md)：17 份小型输入和 28 个初始案例。
