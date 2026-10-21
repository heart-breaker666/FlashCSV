#pragma once

#include "flash_csv/types.hpp"
#include <functional>

namespace flash_csv {

// Statistics（运行统计）：保存读取、匹配、输出的行数以及字节数、线程数和耗时。
// 仅在成功发布结果后返回；行数不含表头，input_bytes 包含表头与 BOM。
struct Statistics {
    std::uint64_t read_rows = 0; // 已读取的数据行数，不含表头。
    std::uint64_t matched_rows = 0; // 通过全部过滤条件的数据行数。
    std::uint64_t output_rows = 0; // 最终写出的数据行/统计组数，不含表头。
    std::uint64_t input_bytes = 0; // 已消费的原始输入字节数，用于计算 MiB/s。
    std::size_t threads = 1; // 实际配置的计算线程数，导出固定为 1。
    double seconds = 0; // 从路径检查到成功发布结果的总耗时，单位为秒。
};

// ExecutionHooks（执行钩子）：提供线程启动和输出阶段的测试回调入口。
// 在指定生命周期位置注入故障，使线程启动和输出失败测试可以稳定复现。
// 常规命令行执行使用空回调；执行期间 hooks 必须保持有效。
struct ExecutionHooks {
    std::function<void(std::size_t)> before_worker; // 启动线程前的测试回调，参数为线程下标。
    std::function<void(const char*)> output_checkpoint; // 输出阶段测试回调，参数为阶段名称。
};

// 组织读取、查询、输出与资源回收，任一失败通过异常交给调用者。
Statistics execute(const Config& config, const ExecutionHooks& hooks = {});
// 生成成功统计文本；main 将其写到 stderr。
std::string report(const Statistics& statistics);

} // namespace flash_csv
