# 验收数据

17 份 CSV 输入、28 个案例，规则以 [设计](../../docs/design.md) 为准。
cases.json 保存输入、参数、退出码和预期表头/记录；由 tests/acceptance.py 执行，并通过 CTest 的 acceptance 目标运行。

每例在项目内构建目录的独立临时测试目录运行：

```text
flash_csv MODE INPUT --output UNIQUE_OUTPUT ARGS...
```

numeric_columns 为从 0 开始的浮点列，按设计容差比较；COUNT、其他字段精确比较。
错误案例的 error_kind 是语义分类，error_line 是原始行号；失败不得留下最终输出。

保留 crlf.csv、bom.csv、no_final_newline.csv 的原始字节。
empty.csv 是零字节错误输入，header_only.csv 合法。bad_ 文件故意不合法。
并发取消、合并超限和 I/O 故障由 core_tests.cpp、engine_tests.cpp 与 acceptance.py 补充验证；
队列等待、部分线程启动失败、发布竞争和清理失败使用可控条件或注入点，不依赖随机调度。
