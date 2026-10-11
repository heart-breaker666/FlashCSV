# FlashCSV

C++17 命令行 CSV 处理引擎，用于学习和简历展示。项目根目录：`/data/fangdashuai/mycode/flash_csv`。开发、构建与生成数据均在此目录内。

当前完成设计与开发前准备，尚无核心代码或 CMake 构建目标。

- [设计与验收](docs/design.md)：功能、数据规则和验收依据。
- [实施顺序](docs/implementation-plan.md)：任务清单与完成条件。
- [开发约定](AGENTS.md)：简短的项目规则。
- [交接记录](docs/HANDOFF.md)：实际状态和下一步。
- [验收数据](tests/fixtures/README.md)：17 份输入、28 个预期案例。

CMake 目标建立后，从本目录构建与验证：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

这些命令目前尚不可执行；目标程序名为 `flash_csv`，用法见设计。
