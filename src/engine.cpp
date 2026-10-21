#include "flash_csv/engine.hpp"
#include "flash_csv/aggregate.hpp"
#include "flash_csv/pipeline.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace flash_csv {
namespace {

// FileBuffer（文件缓冲区）：缓冲输出字节，并将其写入 Linux 文件描述符。
// 将 Linux 文件描述符适配为 ostream 使用的缓冲区，复用 CSV 写出接口。
// 这里只借用 fd，文件关闭与临时路径的生命周期统一由 Output 管理。
class FileBuffer : public std::streambuf {
public:
    FileBuffer(int fd, const ExecutionHooks& hooks) : fd_(fd), hooks_(hooks) {
        setp(buffer_.data(), buffer_.data() + buffer_.size());
    }
protected:
    int sync() override {
        // pbase 是缓冲起点，pptr 是下一个写入位置，两者之差为待写字节数。
        const auto size = static_cast<std::size_t>(pptr() - pbase()); // 本次需要刷出的字节数。
        if (size && hooks_.output_checkpoint) hooks_.output_checkpoint("write");
        std::size_t offset = 0; // 本轮刷出操作中已成功写入的字节数。
        // write 可能只写一部分，也可能被信号中断；循环补齐，EINTR 时重试。
        // 真正失败用 -1 传给流，engine 再通过流状态检查报告错误。
        while (offset < size) {
            const auto written = ::write(fd_, pbase() + offset, size - offset); // 此次写入的字节数，-1 为失败。
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return -1;
            offset += static_cast<std::size_t>(written);
        }
        setp(buffer_.data(), buffer_.data() + buffer_.size());
        return 0;
    }
    int_type overflow(int_type c) override {
        // 缓冲已满时先刷出，再保存当前字符；EOF 只表示请求刷出。
        if (sync() != 0) return traits_type::eof();
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(c);
            pbump(1);
        }
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* source, std::streamsize count) override {
        // 批量写入时按剩余缓冲空间分段复制，减少逐字符调用的开销。
        std::streamsize copied = 0; // 本次调用已经复制进缓冲区的字节数。
        while (copied < count) {
            if (pptr() == epptr() && sync() != 0) break;
            const auto chunk = std::min(count - copied, static_cast<std::streamsize>(epptr() - pptr())); // 本轮可复制长度。
            std::memcpy(pptr(), source + copied, static_cast<std::size_t>(chunk));
            pbump(static_cast<int>(chunk));
            copied += chunk;
        }
        return copied;
    }
private:
    int fd_; // 借用的临时文件描述符，由 Output 负责关闭。
    const ExecutionHooks& hooks_; // 借用的输出故障测试回调。
    std::array<char, 64 * 1024> buffer_{}; // 暂存待刷出的输出字节。
};

// Output（输出管理器）：管理临时结果文件，完成写入、发布和资源清理。
// RAII：对象独占临时文件资源，异常退出时析构负责关闭和尝试删除临时路径。
// 写入全过程先使用临时文件，全部检查成功后才让最终路径可见。
class Output {
public:
    Output(const std::string& destination, const ExecutionHooks& hooks)
        : destination_(destination), hooks_(hooks) {
        auto directory = destination_.parent_path(); // 目标父目录，空时使用当前目录。
        if (directory.empty()) directory = ".";
        // 与目标同目录创建独占临时文件，发布时可以在同一文件系统建立硬链接。
        const auto pattern = (directory / ".flash_csv-XXXXXX").string(); // mkstemp 使用的唯一文件名模板。
        std::vector<char> name(pattern.begin(), pattern.end()); // 供 mkstemp 改写的路径缓冲区，随后补 NUL。
        name.push_back('\0');
        fd_ = ::mkstemp(name.data());
        if (fd_ < 0) throw Error(1, "io", "cannot create output temporary file: " + std::string(std::strerror(errno)));
        // 构造函数抛出时本对象的析构不会执行；后续分配必须单独保护 fd/路径。
        try {
            temporary_ = name.data();
            buffer_ = std::make_unique<FileBuffer>(fd_, hooks_);
            stream_ = std::make_unique<std::ostream>(buffer_.get());
        } catch (...) {
            ::close(fd_);
            ::unlink(name.data());
            fd_ = -1;
            throw;
        }
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output() {
        if (fd_ >= 0) ::close(fd_);
        cleanup();
    }
    std::ostream& stream() { return *stream_; }
    void publish() {
        // flush 检查缓冲写入，close 检查关闭结果，二者成功后才能发布文件。
        // 此流程未调用 fsync，不提供断电持久化保证。
        checkpoint("flush");
        stream_->flush();
        if (!*stream_) throw Error(1, "io", "output flush failed");
        checkpoint("close");
        const int fd = fd_; // 本次需要关闭的描述符，暂存后解除成员的所有权。
        // close 前先解除所有权，失败时析构也不会重复关闭可能已复用的 fd。
        fd_ = -1;
        if (::close(fd) != 0) throw Error(1, "io", "output close failed: " + std::string(std::strerror(errno)));
        checkpoint("publish");
        std::error_code error; // 接收硬链接发布的文件系统错误。
        // 创建目标硬链接拒绝已有路径；即使目标在前期检查后被并发创建也不会覆盖。
        // 发布成功后删除临时名称，目标链接仍指向已经写完的同一文件。
        std::filesystem::create_hard_link(temporary_, destination_, error);
        if (error) throw Error(1, "io", "output publish failed: " + error.message());
        cleanup();
    }
private:
    void checkpoint(const char* stage) {
        if (hooks_.output_checkpoint) hooks_.output_checkpoint(stage);
    }
    void cleanup() noexcept {
        if (temporary_.empty()) return;
        // 清理也可能失败，只报告警告；析构路径不能抛出第二个异常。
        try {
            checkpoint("cleanup");
            std::error_code error; // 接收删除临时路径的错误，不向外抛出。
            std::filesystem::remove(temporary_, error);
            if (error) std::fprintf(stderr, "warning: temporary cleanup failed: %s\n", error.message().c_str());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "warning: temporary cleanup failed: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "warning: temporary cleanup failed\n");
        }
        // 清理只针对临时路径，保留已发布的结果和此前的异常。
        temporary_.clear();
    }
    std::filesystem::path destination_; // 成功后对外可见的最终结果路径。
    std::filesystem::path temporary_; // 本次创建的临时路径，清理后置空。
    const ExecutionHooks& hooks_; // 借用的输出生命周期测试回调。
    int fd_ = -1; // 当前拥有的文件描述符，-1 表示无需再关闭。
    std::unique_ptr<FileBuffer> buffer_; // 适配 fd_ 的输出缓冲区，寿命长于 stream_。
    std::unique_ptr<std::ostream> stream_; // CSV 写出使用的流，借用 buffer_。
};

void validate_paths(const Config& config) {
    std::error_code error; // 接收路径检查错误，供每一步判断。
    const auto input = std::filesystem::status(config.input, error); // 跟随符号链接后的输入文件状态。
    if (error || !std::filesystem::is_regular_file(input))
        throw Error(1, "io", "input must be an accessible regular file");
    // 检查路径自身而非符号链接目标，悬空链接也算已有输出，不能占用。
    const auto output = std::filesystem::symlink_status(config.output, error); // 输出路径自身的状态。
    if (error && error != std::errc::no_such_file_or_directory)
        throw Error(1, "io", "cannot inspect output: " + error.message());
    if (std::filesystem::exists(output)) {
        error.clear();
        const bool same = std::filesystem::equivalent(config.input, config.output, error); // 两路径是否指向同一文件。
        if (!error && same) throw Error(2, "config", "input and output are the same file");
        throw Error(1, "io", "output already exists");
    }
}

void aggregate(CsvReader& reader, const Config& config, const Query& query,
               std::ostream& output, Statistics& stats, const ExecutionHooks& hooks) {
    // 主线程是唯一读取方；有界队列分发批次，工作线程共享只读 Query。
    BatchQueue queue(config.limits.queue_batches); // 主线程与工作线程之间的有界批次队列。
    Failure failure(queue); // 保存首个异常，并协调取消所有处理。
    BatchReader batches(reader, config.limits); // 主线程使用的受限批次读取器。
    // 启动前建好所有局部表，每个线程只写自己的表，因此聚合更新无需加锁。
    std::vector<std::unique_ptr<Aggregator>> locals; // 按工作线程下标排列的独占聚合表。
    for (std::size_t i = 0; i < config.threads; ++i)
        locals.push_back(std::make_unique<Aggregator>(query, config.limits.max_groups));
    std::vector<std::thread> workers; // 已成功启动、结束前必须 join 的线程。
    workers.reserve(config.threads);
    try {
        for (std::size_t i = 0; i < config.threads; ++i) { // i 同时选择工作线程及其局部表。
            if (hooks.before_worker) hooks.before_worker(i);
            // i 按值捕获，确保每个线程固定访问自己的表；其余对象活到 join 后。
            workers.emplace_back([&, i] {
                try {
                    Batch batch; // 当前线程已领取、正在计算的批次。
                    while (queue.pop(batch)) {
                        for (const auto& row : batch.rows) {
                            // 正在处理的批次也逐行检查取消，避免一直计算到批次末尾。
                            if (queue.cancelled()) break;
                            locals[i]->consume(row);
                        }
                        // 等待下一批前释放当前批次，避免空等时继续持有字段内存。
                        batch = Batch{};
                    }
                } catch (...) { failure.capture(std::current_exception()); }
            });
        }
        Batch batch; // 主线程当前读取并准备投递的批次。
        while (!queue.cancelled() && batches.next(batch)) {
            if (!queue.push(std::move(batch))) break;
        }
        // 正常读完只关闭队列，让已入队批次被处理完毕。
        queue.close();
    } catch (...) { failure.capture(std::current_exception()); }
    // 读取失败、计算失败或部分线程启动失败均先取消，再回收所有已启动线程。
    // 必须 join 后再抛出，否则线程仍会引用这里即将析构的局部对象。
    for (auto& worker : workers) worker.join();
    failure.rethrow();
    stats.read_rows = batches.rows_read();
    // join 后局部表不再变化，主线程合并并再次检查最终表的分组上限。
    // 浮点加法受合并顺序影响，多线程结果不承诺逐字节相同。
    Aggregator merged(query, config.limits.max_groups); // 汇总所有局部表的最终聚合器。
    for (const auto& local : locals) merged.merge(*local);
    stats.matched_rows = merged.matched();
    stats.output_rows = merged.write(output);
}

} // namespace

Statistics execute(const Config& config, const ExecutionHooks& hooks) {
    // execute 也可能由测试或库调用者直接使用，不能仅依赖 CLI 的容量校验。
    if (!config.threads || config.threads > 32 || !config.limits.max_record_bytes ||
        config.limits.batch_bytes < config.limits.max_record_bytes || !config.limits.batch_rows ||
        !config.limits.queue_batches || !config.limits.max_groups)
        throw Error(2, "config", "invalid execution capacities");
    // 使用单调时钟测量耗时，避免系统时间校准影响统计。
    const auto start = std::chrono::steady_clock::now(); // 总耗时的起点。
    validate_paths(config);
    // 二进制模式保留原始换行/BOM 字节，CSV 解析和吞吐统计自行处理它们。
    std::ifstream input(config.input, std::ios::binary); // 拥有输入文件资源的二进制流。
    if (!input) throw Error(1, "io", "cannot open input");
    CsvReader reader(input, config.limits.max_record_bytes); // 借用 input 的 CSV 解析器。
    // 输入流先于 reader 创建，Query 先于工作线程创建，借用的对象活到使用结束。
    // 先验证查询再创建临时输出，尽早拒绝不存在的列等配置错误。
    const Query query(config, reader.header()); // 已绑定表头、可在线程间只读共享的查询。
    Output output(config.output, hooks); // 管理临时写入、最终发布及异常清理。
    Statistics stats; // 累计本次执行的行数、字节数和耗时。
    stats.threads = config.mode == Mode::Aggregate ? config.threads : 1;
    write_record(output.stream(), query.output_header());
    // 导出在主线程逐行处理和写入，天然保留输入顺序；聚合走并发流水线。
    if (config.mode == Mode::Export) {
        Row row; // 导出循环当前读取、过滤并写出的数据记录。
        while (reader.next(row)) {
            add_count(stats.read_rows, 1, row.line);
            if (!query.matches(row)) continue;
            add_count(stats.matched_rows, 1, row.line);
            write_record(output.stream(), query.project(row));
            add_count(stats.output_rows, 1, row.line);
        }
    } else aggregate(reader, config, query, output.stream(), stats, hooks);
    stats.input_bytes = reader.bytes_read();
    // 发布成功才返回统计，让 main 只在整个执行成功时打印成功报告。
    output.publish();
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return stats;
}

std::string report(const Statistics& stats) {
    std::ostringstream output; // 拼接供 main 写到 stderr 的成功统计文本。
    output << std::setprecision(8) << "read_rows=" << stats.read_rows
           << "\nmatched_rows=" << stats.matched_rows
           << "\noutput_rows=" << stats.output_rows
           << "\nthreads=" << stats.threads
           << "\nseconds=" << stats.seconds
           << "\nrows/s=" << (stats.seconds ? static_cast<double>(stats.read_rows) / stats.seconds : 0)
           << "\nMiB/s=" << (stats.seconds ? static_cast<double>(stats.input_bytes) / (1024 * 1024) / stats.seconds : 0)
           << "\ninput_bytes=" << stats.input_bytes;
    return output.str();
}

} // namespace flash_csv
