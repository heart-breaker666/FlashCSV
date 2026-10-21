#pragma once

#include "flash_csv/types.hpp"
#include <array>
#include <istream>
#include <ostream>

namespace flash_csv {

// CsvReader（CSV 读取器）：逐条读取、校验并解码 CSV 记录。
// 构造时读取并验证表头；之后逐条解码数据。仅借用输入流，不拥有它。
// 本解析器不支持字段内换行，因此记录行号就是原始物理行号。
class CsvReader {
public:
    CsvReader(std::istream& input, std::size_t max_record_bytes);
    const std::vector<std::string>& header() const { return header_; }
    // EOF 返回 false；格式、容量或 I/O 错误抛出 Error，不伪装成 EOF。
    bool next(Row& row);
    std::uint64_t bytes_read() const { return bytes_read_; }
private:
    // peek 查看当前字节，take 消费字节；缓冲区不足时由 peek 重新填充。
    int peek();
    int take();
    bool record(Row& row);
    std::istream& input_; // 借用的输入流，必须在读取期间保持有效。
    std::size_t limit_; // 单条原始记录的字节上限，表头同样受限。
    // 固定大小的 I/O 缓冲区可跨多条记录使用，与单条记录上限分别控制。
    std::array<char, 64 * 1024> buffer_{};
    std::size_t position_ = 0; // 缓冲区内下一个待消费字节的下标。
    std::size_t available_ = 0; // 当前缓冲区中有效字节数。
    bool eof_ = false; // 输入流已到 EOF；缓冲区仍可能有未消费字节。
    std::uint64_t next_line_ = 1; // 下一条记录的原始行号，表头从 1 开始。
    std::uint64_t bytes_read_ = 0; // 已消费的输入字节数，含 BOM、表头和换行。
    std::vector<std::string> header_; // 已验证的列名，用于列数校验和错误定位。
};

// 输出一条记录并补 LF；对特殊字符加引号，内部双引号写成两个双引号。
void write_record(std::ostream& output, const std::vector<std::string>& fields);

} // namespace flash_csv
