#pragma once

#include "flash_csv/csv.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>

namespace flash_csv {

// BatchReader（批次读取器）：从 CSV 读取器中收集记录，按容量限制组成批次。
// 将记录凑成同时受字节数和行数约束的批次；借用 reader 和 limits。
class BatchReader {
public:
    BatchReader(CsvReader& reader, const Limits& limits);
    bool next(Batch& batch);
    std::uint64_t rows_read() const { return rows_read_; }
private:
    CsvReader& reader_; // 主线程使用的 CSV 读取器。
    const Limits& limits_; // 借用的批次容量配置。
    // 已读出但放不进当前批次的一条记录，下次 next 优先使用它。
    std::optional<Row> pending_;
    std::uint64_t rows_read_ = 0; // 已从 reader 读出的数据行数，含 pending 中的记录。
};

// BatchQueue（批次队列）：在线程间传递数据批次，协调等待、正常结束和取消。
// 有界生产者/消费者队列：满时阻塞读取方，将背压传回输入端。
// close 停止入队并允许排空；cancel 丢弃队列内容并唤醒所有等待者。
class BatchQueue {
public:
    explicit BatchQueue(std::size_t capacity);
    // push/pop 返回 false 表示无法继续投递/领取，无需用异常表示正常结束。
    bool push(Batch batch);
    bool pop(Batch& batch);
    void close();
    void cancel();
    bool cancelled() const { return cancelled_.load(std::memory_order_relaxed); }
private:
    // State（队列状态）：区分可入队、关闭后排空和取消处理三种状态。
    enum class State { Open, Closed, Cancelled };
    std::size_t capacity_; // 最多允许排队的批次数。
    State state_ = State::Open; // 队列生命周期状态，由 mutex_ 保护。
    // 仅供处理记录时快速检查是否停止；队列状态与内容仍由 mutex_ 保护。
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_; // 保护队列内容与 state_ 的互斥锁。
    std::condition_variable readable_; // 消费者等待有批次可领或队列结束。
    std::condition_variable writable_; // 生产者等待有空位可用或队列结束。
    std::deque<Batch> queue_; // 尚未领取的批次，按入队顺序存放。
};

// Failure（失败处理器）：保存首个异常并取消队列，供主线程重新抛出。
// 保存第一个被捕获的异常并取消队列，让工作线程异常传回主线程。
// 多线程捕获顺序不固定，因此不保证报告行号最小的错误。
class Failure {
public:
    explicit Failure(BatchQueue& queue) : queue_(queue) {}
    void capture(std::exception_ptr error);
    void rethrow();
private:
    BatchQueue& queue_; // 捕获异常时要取消的共享队列。
    std::mutex mutex_; // 保护 first_，避免多个线程同时改写。
    std::exception_ptr first_; // 首个被捕获的异常，供主线程重新抛出。
};

} // namespace flash_csv
