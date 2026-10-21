# FlashCSV 从零学习指南

这份指南先帮助你理解项目和使用程序。你可以暂时不读 C++ 源码，先亲手完成几个小任务。
掌握用法后，再读[代码讲解](code-walkthrough.md)，理解程序怎样实现这些功能。

本文用当前代码和仓库自带的数据教学。完整功能约定以[设计与验收](design.md)为准。

## 1. 这个项目到底做什么

FlashCSV 是一个在 Linux 终端中运行的小工具：**给它一个 CSV 文件和处理要求，它生成一个结果 CSV 文件。**

例如，你有很多城市的交易记录，想完成以下工作：

| 你想做的事 | 在 FlashCSV 中使用的功能 |
|---|---|
| 找出金额大于 100 的记录 | 过滤：`--where` |
| 只保留城市这一列 | 选择列：`--select` |
| 把金额列放到城市列前面 | 按指定顺序选择列 |
| 算出全部记录的总金额 | 整体聚合：`aggregate --agg sum amount` |
| 分别算出每个城市的总金额 | 分组聚合：再加 `--group-by city` |
| 尝试让多个计算线程参与统计 | 聚合时加 `--threads` |

项目还有一个学习用途：把文件读取、字符串解析、容器、异常处理和线程协作，连接成一个可运行、可测试的 C++ 程序。
你读代码时会看到，这些技术都在解决一个具体问题，而不是孤立的语法练习。

### 1.1 CSV 是什么

CSV 可以理解为“用文本保存的表格”。打开 [sales.csv](../tests/fixtures/sales.csv)，内容是：

```csv
city,amount
上海,100
北京,80
上海,200
```

把它画成表格，就是：

| city：城市 | amount：金额 |
|---|---:|
| 上海 | 100 |
| 北京 | 80 |
| 上海 | 200 |

第一行 `city,amount` 是**表头**，告诉程序两列分别叫什么。下面三行是**数据记录**。
为了方便理解，可以把每条记录看作一笔订单。这个例子共有三笔订单，合计金额 `100 + 80 + 200 = 380`。

逗号通常用来分隔列。如果字段本身包含逗号，就要用双引号包围，例如：

```csv
city,note
"North,Zone","said ""hello"""
```

第二行有两个字段：城市是 `North,Zone`，备注是 `said "hello"`。
这也是程序需要专门解析 CSV 的原因：简单地“遇到逗号就切开”会把城市名切错。

### 1.2 输入、处理、输出

```mermaid
flowchart LR
    A[输入 CSV] --> B[按要求筛选或统计]
    B --> C[新的结果 CSV]
```

原始 CSV 提供数据，命令行参数说明要做什么，结果文件保存答案。
执行程序后，你看到的终端文字通常是处理统计；真正的表格结果要打开 `--output` 指定的文件查看。

### 1.3 为什么要分批处理

大文件可能装不进内存。FlashCSV 逐步读取记录；聚合时，把有限数量的记录组成批次，交给计算线程处理。
已经统计过的记录可以释放，累计结果留在统计表里。

假设文件有一千万条记录，却只有两个城市，程序主要需要维护两个城市的统计结果。
如果每条记录都有一个不同的分组值，就需要很多组的状态。因此，大文件能否处理还取决于记录大小、分组数和容量设置。
这里的“流式处理”表示分批读取和处理，并不表示所有情况下都只占固定的几个 MiB 内存。

## 2. 先把程序运行起来

以下命令在 Linux 的 Bash 终端执行。命令前面没有 `$` 提示符，可以直接复制。

先进入项目目录：

```bash
cd FlashCSV
```

### 2.1 构建：把 C++ 源码变成可执行程序

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release -j2
```

这两步分别做什么：

| 命令中的部分 | 含义 |
|---|---|
| `cmake -S .` | 从当前目录读取项目的构建说明 |
| `-B build/release` | 将构建文件放在 `build/release/` |
| `Release` | 使用适合正常运行和性能测量的构建配置 |
| `BUILD_TESTING=ON` | 同时建立测试程序 |
| `cmake --build ...` | 真正编译、链接程序 |
| `-j2` | 最多同时执行两个构建任务；这是编译并行度 |

构建成功后，程序路径是 `./build/release/flash_csv`。
开头的 `./` 表示从当前目录寻找它。这里的 `-j2` 和之后统计时使用的 `--threads 2` 是两件不同的事。

当前环境已验证 GCC 11.5 和 Python 3.9。测试依赖 Python；核心程序用 C++17 编写。

### 2.2 看帮助

```bash
./build/release/flash_csv --help
```

帮助会列出两种模式、可用参数和默认容量。现在只需记住两种模式：

| 模式 | 作用 | 输出的一行代表什么 |
|---|---|---|
| `export` | 筛选记录、选择列、输出明细 | 一条通过筛选的原始记录 |
| `aggregate` | 计算数量、总和、最小值、最大值、平均值 | 一个分组的统计结果，或全部记录的统计结果 |

一条处理命令的基本结构是：

```text
程序路径 模式 输入文件 --output 结果文件 其他处理要求
```

例如 `--output` 是参数名，后面的路径是这个参数的值；`--where` 后面四个值一起描述一个筛选条件。

### 2.3 建立本次练习的输出目录

```bash
mkdir -p reports
lesson_dir=$(mktemp -d reports/lesson-XXXXXX)
```

第一行确保 `reports/` 存在。第二行建立一个新的练习目录，并把它的路径记在变量 `lesson_dir` 中，例如 `reports/lesson-a1b2c3`。
后续的 `"$lesson_dir/01-filter.csv"` 表示这个目录里的结果文件。

请在同一个终端中继续练习，这样 `lesson_dir` 变量仍然有效。如果换了终端，从本节的设置命令重新开始。
每次重新设置会得到新目录，方便保存不同轮次的结果。
后面的长命令会分成多行：行末的反斜杠 `\` 表示命令在下一行继续；复制时保留它，且不要在它后面添加空格。

## 3. 第一轮练习：导出明细

### 3.1 原样导出

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output "$lesson_dir/00-all.csv"
cat "$lesson_dir/00-all.csv"
```

`cat` 用来显示文件内容。预期结果：

```csv
city,amount
上海,100
北京,80
上海,200
```

这一步让你确认“输入文件 → 程序 → 结果文件”这条路径已经跑通。

### 3.2 找出金额大于 100 的记录

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output "$lesson_dir/01-filter.csv" \
  --where amount num gt 100
cat "$lesson_dir/01-filter.csv"
```

预期结果：

```csv
city,amount
上海,200
```

`--where amount num gt 100` 的四个部分可以这样读：

| 部分 | 含义 |
|---|---|
| `amount` | 检查金额列 |
| `num` | 把字段当作数字比较 |
| `gt` | greater than，大于 |
| `100` | 和 100 比较 |

金额恰好为 100 的记录没有留下，因为要求是“大于 100”。

### 3.3 只留下城市列

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output "$lesson_dir/02-city.csv" \
  --select city
cat "$lesson_dir/02-city.csv"
```

预期结果：

```csv
city
上海
北京
上海
```

这里仍然是三条明细。**选择列不会自动去重**，所以“上海”出现两次。

### 3.4 调整列的顺序

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output "$lesson_dir/03-reorder.csv" \
  --select amount --select city
cat "$lesson_dir/03-reorder.csv"
```

预期结果：

```csv
amount,city
100,上海
80,北京
200,上海
```

`--select` 可以多次出现，输出列的顺序就是你写参数的顺序。

### 3.5 同时满足两个条件

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output "$lesson_dir/04-two-conditions.csv" \
  --where city str eq 上海 \
  --where amount num ge 100
cat "$lesson_dir/04-two-conditions.csv"
```

预期结果：

```csv
city,amount
上海,100
上海,200
```

两个条件分别是“城市等于上海”和“金额大于或等于 100”。多次 `--where` 表示这些条件都要满足。
`str` 表示文字比较，`eq` 表示相等，`ge` 表示大于或等于。

常用比较词：

| 操作词 | 含义 | 可用于哪种比较 |
|---|---|---|
| `eq` | 等于 | `str`、`num` |
| `ne` | 不等于 | `str`、`num` |
| `gt` | 大于 | `num` |
| `ge` | 大于或等于 | `num` |
| `lt` | 小于 | `num` |
| `le` | 小于或等于 | `num` |

## 4. 第二轮练习：统计结果

### 4.1 每个城市分别统计

```bash
./build/release/flash_csv aggregate tests/fixtures/sales.csv \
  --output "$lesson_dir/05-grouped.csv" \
  --group-by city \
  --agg count \
  --agg sum amount \
  --agg min amount \
  --agg max amount \
  --agg avg amount \
  --threads 2
cat "$lesson_dir/05-grouped.csv"
```

预期结果：

```csv
city,count,sum(amount),min(amount),max(amount),avg(amount)
上海,2,300,100,200,150
北京,1,80,80,80,80
```

`--group-by city` 表示把城市相同的记录放在一组，然后分别计算：

| 函数 | 中文含义 | 上海这一组怎样计算 |
|---|---|---|
| `count` | 记录数 | 有两条记录，所以是 2 |
| `sum amount` | 金额总和 | `100 + 200 = 300` |
| `min amount` | 最小金额 | 100 |
| `max amount` | 最大金额 | 200 |
| `avg amount` | 平均金额 | `300 / 2 = 150` |

`count` 后面不写列名；其他四个函数要说明统计哪一列。
聚合前也可以加 `--where`，程序会先判断记录是否通过筛选，再让它参与统计。

### 4.2 全部记录合在一起统计

```bash
./build/release/flash_csv aggregate tests/fixtures/sales.csv \
  --output "$lesson_dir/06-overall.csv" \
  --agg count --agg sum amount --agg avg amount
cat "$lesson_dir/06-overall.csv"
```

预期结果：

```csv
count,sum(amount),avg(amount)
3,380,126.66666666666667
```

这次省略了 `--group-by`，因此得到一个整体统计结果。数量为 3，总和为 380，平均值为 `380 / 3`。
平均值的小数位较多，是程序保留浮点精度的表现。

### 4.3 没有记录满足条件时

```bash
./build/release/flash_csv aggregate tests/fixtures/sales.csv \
  --output "$lesson_dir/07-empty-overall.csv" \
  --where amount num gt 1000 \
  --agg count --agg sum amount --agg min amount --agg max amount --agg avg amount
cat "$lesson_dir/07-empty-overall.csv"
```

预期结果：

```csv
count,sum(amount),min(amount),max(amount),avg(amount)
0,0,,,
```

整体统计仍能回答“有多少条、合计多少”，所以数量和总和是 0；没有数据可用于确定最小、最大和平均值，对应字段为空。
如果使用分组统计，没有匹配记录就没有分组，结果只含表头。导出没有匹配记录时也只含表头。

## 5. 文字比较和数字比较为什么要分开

在 CSV 中，程序首先读到的是文字。例如 `001` 是三个字符，`100.0` 是五个字符。

| 比较方式 | `001` 和 `1` | `100` 和 `100.0` |
|---|---|---|
| `str`：比较文字内容 | 不相等 | 不相等 |
| `num`：转换成数字再比较 | 相等 | 相等 |

编号、城市和备注通常用 `str`；金额、数量通常用 `num`。
数值比较需要能转换为合法数字。空值、`abc`、带空格的 ` 100` 等内容会导致数值处理失败。
字段中的空格会保留，程序不会替你删除它们。

### 5.1 过滤条件的顺序也有意义

[short_circuit.csv](../tests/fixtures/short_circuit.csv) 中有一条北京记录的金额为 `abc`。
先选上海，再检查金额，可以成功：

```bash
./build/release/flash_csv export tests/fixtures/short_circuit.csv \
  --output "$lesson_dir/08-short-circuit.csv" \
  --select id \
  --where city str eq 上海 \
  --where amount num ge 100
cat "$lesson_dir/08-short-circuit.csv"
```

预期结果：

```csv
id
001
003
```

北京那条记录已经不满足第一个条件，程序就不再检查它的金额。这叫**短路**。
如果把金额条件放在最前面，程序会尝试转换 `abc`，并报告数据错误。
CSV 本身的结构检查总会先执行；短路只能跳过后续条件或聚合所需的数值转换。

## 6. 终端上那串统计信息怎么看

成功时，你可能看到这种格式：

```text
read_rows=3
matched_rows=1
output_rows=1
threads=1
seconds=...
rows/s=...
MiB/s=...
input_bytes=44
```

这里的 `...` 是教学占位符，真实程序会打印数字；耗时和吞吐会随运行环境变化。
上面的固定计数对应第 3.2 节的过滤例子。

| 字段 | 意思 |
|---|---|
| `read_rows` | 读到的数据记录数，不含表头 |
| `matched_rows` | 通过过滤的记录数 |
| `output_rows` | 输出的数据行数，不含表头 |
| `threads` | 计算线程数；导出显示 1 |
| `seconds` | 本次执行的总耗时，单位为秒 |
| `rows/s` | 每秒读取的数据记录数 |
| `MiB/s` | 每秒处理的输入字节数，以 MiB 表示；1 MiB = 1,048,576 字节 |
| `input_bytes` | 输入字节数，包含表头和行结束符 |

分组统计中，`matched_rows=3`、`output_rows=2` 很正常：三条明细被汇总成两个城市的结果。
聚合命令的线程数不包含负责读文件的主线程。即使 `--threads 1`，当前实现也会启动一个计算线程，与主线程通过队列协作。

统计和错误写入标准错误流 `stderr`。表格结果写到 `--output` 的文件，不会因为加上 `> other.csv` 就改为那个文件。
如果需要保存终端统计，可以使用：

```bash
./build/release/flash_csv export tests/fixtures/sales.csv \
  --output "$lesson_dir/09-with-log.csv" \
  2> "$lesson_dir/09-with-log.stderr"
cat "$lesson_dir/09-with-log.stderr"
```

`2>` 是 Bash 的写法，表示把 `stderr` 保存到指定文件。

## 7. 出错时怎么处理

程序要求输出目录已经存在，且目标结果文件还不存在。
重复执行同一个命令时，如果它指向先前生成的文件，会得到 `output already exists`。
可以改用新的结果文件名，或者重新建立一个练习目录继续操作。

| 现象或报错 | 常见原因 | 检查方法 |
|---|---|---|
| 找不到 `flash_csv` | 当前目录不对，或还没构建成功 | 确认已经 `cd` 到项目目录，检查构建输出 |
| `unknown column` | 列名和表头不一致 | 用 `head -n 1` 查看输入表头；中文、大小写和空格都要对应 |
| `invalid_number line 3 column 'amount'` | 第 3 个物理行的金额不是合法数字 | 查看原文件第 3 行；表头是第 1 行 |
| `field_count` | 某条记录的字段数量不匹配 | 检查是否缺了字段，或包含逗号的字段是否正确加了引号 |
| `csv_syntax` | 引号位置或行结束形式有问题 | 对照 CSV 格式和错误行检查 |
| `unsupported_multiline` | 双引号字段中出现换行 | 当前项目要求字段内容位于单个物理行内 |
| `record_limit` | 某一条记录太大 | 查看该行；确有需要时调整 `--max-record-bytes` 及批次容量 |
| `group_limit` | 不同分组值的数量超限 | 检查分组列，或在内存允许时调整 `--max-groups` |

执行一条命令后，可以立即用 `echo $?` 看退出码：0 表示成功，2 表示参数或查询配置错误，1 表示数据、容量或 I/O 等运行错误。
不要把看到错误信息的运行当作一次成功统计。

## 8. 等理解基础用法后，再了解容量和性能

第一轮学习可以使用默认容量，先理解结果。需要处理更大数据时，再读[设计第 5 节](design.md#5-架构与容量)和[代码讲解的容量分析](code-walkthrough.md#11-容量与复杂度怎样理解)。

`--max-record-bytes` 管一条记录的原始大小，`--batch-bytes` 和 `--batch-rows` 管一批记录的大小，
`--queue-batches` 管队列中等待的批次数，`--max-groups` 管每张聚合表的组数。
这些参数控制不同资源，批次字节上限也不等于程序的实际内存占用。

线程数只用于 `aggregate`。更多计算线程可能降低耗时，也会增加调度、合并和局部表的开销。
用三条记录测试线程数，主要能看到功能是否一致；比较性能应使用较大的数据和多次测量。

先看已有[基准报告](../reports/baseline-20261012/summary.md)，理解表里的行数、组数、耗时和 RSS。
RSS 表示进程实际驻留在内存中的部分；基准中的峰值 RSS 由 GNU time 测量，程序自己的成功统计不包含这个字段。

准备好做性能实验后，可以按 [README](../README.md) 使用 `tools/generate_data.py` 和 `tools/benchmark.py`。
完整基准会生成数据并反复运行；`--large` 还会在磁盘允许时生成大于机器内存的输入，适合放在学习后期执行。

## 9. 学习路线和自测

建议按这个顺序推进：

1. 用纸笔算出三条销售记录的总和、分组数量和平均值。
2. 完成第 2–4 节的构建与命令练习，比较你算的答案和输出文件。
3. 阅读第 5–7 节，解释文字/数字比较、短路、统计字段和错误定位。
4. 阅读[代码讲解](code-walkthrough.md)的入口、值类型、CSV、查询和聚合部分。
5. 再读批次、队列、线程回收和文件发布，最后对照测试与性能报告。

练习时先预测答案，再运行；以下题目均使用 `sales.csv`：

| 题目 | 先想一想 |
|---|---|
| 把第 3.2 节的 `gt` 改成 `ge` | 哪些记录会留下？ |
| 只筛选北京，再进行整体 `count` 和 `sum amount` | 输出几个统计结果？数值是什么？ |
| 选择城市列后，为什么仍有两个上海？ | 明细导出和分组统计有什么区别？ |
| 把第 4.1 节的线程数改成 1 或 4，使用新结果文件 | 哪些业务结果应该一致？ |

参考答案：第一题留下上海的 100 和 200；第二题输出一条整体结果，数量 1、总和 80；第三题选择列保留明细，不会合并相同城市；
第四题分组、计数、最小和最大值一致，这个小例子的总和与平均值也一致。一般浮点求和的末尾数位可能受计算顺序影响，详见代码讲解。

学习到这里，你应该能够自己回答三件事：**输入数据是什么、命令要求做什么、输出的一行代表什么。**
接下来的代码讲解，会把这三件事对应到真实的数据结构和函数调用。
