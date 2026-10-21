#include "flash_csv/csv.hpp"
#include <unordered_set>

namespace flash_csv {

CsvReader::CsvReader(std::istream& input, std::size_t max_record_bytes)
    : input_(input), limit_(max_record_bytes) {
    if (limit_ == 0) throw Error(2, "config", "record limit must be positive");
    // 首次 peek 会填满缓冲区或读完短文件，足以检查开头的三个 BOM 字节。
    // BOM 计入输入统计，但跳过它后才解析表头，不占表头的记录字节额度。
    if (peek() == 0xef && available_ >= 3 &&
        static_cast<unsigned char>(buffer_[1]) == 0xbb &&
        static_cast<unsigned char>(buffer_[2]) == 0xbf) {
        position_ = 3;
        bytes_read_ = 3;
    }
    // 表头也走同一套受限解析；先验证列名，再将字段所有权交给 header_。
    Row row; // 暂存解析后的表头记录。
    if (!record(row)) throw Error(1, "missing_header", "input has no header");
    std::unordered_set<std::string> names; // 已出现的列名，用于检测重复表头。
    for (const auto& name : row.fields) {
        if (name.empty()) throw Error(1, "empty_header", "empty column name", 1);
        if (!names.insert(name).second)
            throw Error(1, "duplicate_header", "duplicate column name", 1, name);
    }
    header_ = std::move(row.fields);
}

int CsvReader::peek() {
    if (position_ == available_) {
        if (eof_) return -1;
        input_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        available_ = static_cast<std::size_t>(input_.gcount());
        position_ = 0;
        if (input_.bad() || (input_.fail() && !input_.eof()))
            throw Error(1, "io", "input read failed", next_line_);
        eof_ = input_.eof();
        if (available_ == 0) return -1;
    }
    // 转为无符号字节再提升为 int，避免 0xff 被有符号 char 误当成 EOF(-1)。
    return static_cast<unsigned char>(buffer_[position_]);
}

int CsvReader::take() {
    const int c = peek(); // 当前输入字节；-1 表示没有字节可读。
    if (c != -1) {
        ++position_;
        add_count(bytes_read_, 1, next_line_);
    }
    return c;
}

bool CsvReader::record(Row& row) {
    // State（字段解析状态）：记录当前位置，控制字段内容、引号和分隔符的解析。
    // Start: 字段开头；Plain: 普通字段；Quoted: 引号内；Closed: 刚读到引号。
    // Closed 还不能立即结束字段：后续另一个引号表示转义的双引号。
    enum class State { Start, Plain, Quoted, Closed };
    State state = State::Start; // 当前字段的解析状态。
    std::size_t raw_bytes = 0; // 本条记录已消费的原始字节数，不含行结束符。
    row = Row{next_line_, {}};
    std::string field; // 正在构造的解码字段，结束后移动到 row.fields。
    // 当前已完成的字段数就是待解析字段的下标；表头阶段还没有列名可用。
    auto column = [&] {
        return row.fields.size() < header_.size() ? header_[row.fields.size()] : std::string{};
    };
    for (;;) {
        const int c = take(); // 本轮消费的输入字节，或 EOF 标记 -1。
        if (c == -1) {
            if (state == State::Quoted)
                throw Error(1, "csv_syntax", "unclosed quoted field", row.line, column());
            // 真正无数据的 EOF 不生成记录；非空的末行可没有行结束符。
            if (raw_bytes == 0) return false;
            row.fields.push_back(std::move(field));
            return true;
        }
        // 先处理 LF/CRLF，保证它们不计入记录上限；引号内换行明确报错。
        if (c == '\n' || c == '\r') {
            if (state == State::Quoted)
                throw Error(1, "unsupported_multiline", "newline inside quoted field", row.line, column());
            if (c == '\r' && take() != '\n')
                throw Error(1, "csv_syntax", "standalone CR", row.line, column());
            row.fields.push_back(std::move(field));
            add_count(next_line_, 1, row.line);
            return true;
        }
        // 在追加字段内容前限制原始字节数，包括分隔逗号及转义所用的引号。
        if (raw_bytes == limit_)
            throw Error(1, "record_limit", "record exceeds max-record-bytes", row.line, column());
        ++raw_bytes;
        // 引号内的逗号属于内容；其余状态下逗号结束字段，包括空字段。
        if (state == State::Quoted) {
            if (c == '"') state = State::Closed;
            else field.push_back(static_cast<char>(c));
        } else if (c == ',') {
            row.fields.push_back(std::move(field));
            field.clear();
            state = State::Start;
        } else if (c == '"') {
            // 连续的两个双引号解码为一个，再回到引号内继续读取。
            if (state == State::Closed) {
                field.push_back('"');
                state = State::Quoted;
            } else if (state == State::Start) state = State::Quoted;
            else throw Error(1, "csv_syntax", "quote inside unquoted field", row.line, column());
        } else {
            if (state == State::Closed)
                throw Error(1, "csv_syntax", "characters after closing quote", row.line, column());
            field.push_back(static_cast<char>(c));
            state = State::Plain;
        }
    }
}

bool CsvReader::next(Row& row) {
    if (!record(row)) return false;
    // 完整记录先通过格式和列数检查，查询过滤不能跳过这些结构错误。
    if (row.fields.size() != header_.size())
        throw Error(1, "field_count", "expected " + std::to_string(header_.size()) +
                    " fields, got " + std::to_string(row.fields.size()), row.line);
    return true;
}

void write_record(std::ostream& output, const std::vector<std::string>& fields) {
    for (std::size_t i = 0; i < fields.size(); ++i) { // i 为当前输出字段下标。
        if (i) output.put(',');
        const auto& field = fields[i]; // 当前待转义写出的字段内容。
        // 单列空字符串也写成带引号的空字段，避免被其他 CSV 工具当作空白行。
        if (field.find_first_of(",\"\r\n") == std::string::npos &&
            !(fields.size() == 1 && field.empty())) {
            output.write(field.data(), static_cast<std::streamsize>(field.size()));
        } else {
            output.put('"');
            for (char c : field) {
                if (c == '"') output.put('"');
                output.put(c);
            }
            output.put('"');
        }
    }
    output.put('\n');
    if (!output) throw Error(1, "io", "output write failed");
}

} // namespace flash_csv
