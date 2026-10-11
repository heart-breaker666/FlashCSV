# 验收数据

17 份 CSV 输入、28 个案例，规则以 [设计](../../docs/design.md) 为准。
cases.json 保存输入、参数、退出码和预期表头/记录；这是测试向量，尚无执行器。

后续每例在项目内独立测试目录运行：

```text
flash_csv MODE INPUT --output UNIQUE_OUTPUT ARGS...
```

numeric_columns 为从 0 开始的浮点列，按设计容差比较；COUNT、其他字段精确比较。
错误案例的 error_kind 是语义分类，error_line 是原始行号；失败不得留下最终输出。

保留 crlf.csv、bom.csv、no_final_newline.csv 的原始字节。
empty.csv 是零字节错误输入，header_only.csv 合法。bad_ 文件故意不合法。
并发取消、合并超限和 I/O 故障另由实现阶段测试，不依赖线程随机调度。
