#include "flash_csv/pipeline.hpp"

namespace flash_csv {

BatchReader::BatchReader(CsvReader& reader, const Limits& limits)
    : reader_(reader), limits_(limits) {
    if (limits_.batch_bytes < limits_.max_record_bytes || !limits_.batch_rows || !limits_.batch_bytes)
        throw Error(2, "config", "invalid batch limits");
}

bool BatchReader::next(Batch& batch) {
    batch = Batch{};
    while (batch.rows.size() < limits_.batch_rows) {
        Row row; // 本轮准备加入批次的完整记录。
        // pending 已在前一次调用读出并计数，移入新批次时不能再计一次。
        if (pending_) { row = std::move(*pending_); pending_.reset(); }
        else {
            if (!reader_.next(row)) break;
            add_count(rows_read_, 1, row.line);
        }
        const auto bytes = row.field_bytes(); // 该记录解码后的字段内容字节数。
        if (bytes > limits_.batch_bytes)
            throw Error(1, "batch_limit", "row exceeds batch byte limit", row.line);
        // 完整记录不拆分；当前批次放不下时，仅暂存这一条供下次使用。
        if (bytes > limits_.batch_bytes - batch.bytes) {
            pending_ = std::move(row);
            break;
        }
        batch.bytes += bytes;
        // 用移动转移字段字符串；外层行数上限同时约束零字节的空记录。
        batch.rows.push_back(std::move(row));
        if (batch.bytes == limits_.batch_bytes) break;
    }
    return !batch.rows.empty();
}

BatchQueue::BatchQueue(std::size_t capacity) : capacity_(capacity) {
    if (!capacity_) throw Error(2, "config", "queue capacity must be positive");
}

bool BatchQueue::push(Batch batch) {
    std::unique_lock<std::mutex> lock(mutex_); // 等待空位时可暂时释放的队列锁。
    // wait 等待时释放锁，醒来后重新持锁检查谓词，以应对虚假唤醒。
    // 状态变化也必须能结束等待，否则取消时满队列的生产者会卡住。
    writable_.wait(lock, [&] { return state_ != State::Open || queue_.size() < capacity_; });
    if (state_ != State::Open) return false;
    queue_.push_back(std::move(batch));
    readable_.notify_one();
    return true;
}

bool BatchQueue::pop(Batch& batch) {
    std::unique_lock<std::mutex> lock(mutex_); // 等待批次时可暂时释放的队列锁。
    // 关闭后仍可领取剩余批次；取消则立即停止，即使队列之前有数据。
    readable_.wait(lock, [&] { return state_ != State::Open || !queue_.empty(); });
    if (state_ == State::Cancelled || queue_.empty()) return false;
    batch = std::move(queue_.front());
    queue_.pop_front();
    writable_.notify_one();
    return true;
}

void BatchQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_); // 使关闭状态更新与队列操作互斥。
    // 只允许 Open -> Closed，防止读取线程的正常结束覆盖已有的取消状态。
    if (state_ == State::Open) state_ = State::Closed;
    readable_.notify_all();
    writable_.notify_all();
}

void BatchQueue::cancel() {
    std::lock_guard<std::mutex> lock(mutex_); // 在同一次互斥操作中更新状态并清空队列。
    state_ = State::Cancelled;
    // 原子标志只用于通知工作循环停止，不发布其他数据，所以用 relaxed。
    // 队列内容的可见性和互斥仍由 mutex_ 保证。
    cancelled_.store(true, std::memory_order_relaxed);
    queue_.clear();
    // 消费者可能在等数据，生产者可能在等空间，两类等待者都须唤醒。
    readable_.notify_all();
    writable_.notify_all();
}

void Failure::capture(std::exception_ptr error) {
    {
        std::lock_guard<std::mutex> lock(mutex_); // 保护首个异常的选择。
        if (!first_) first_ = std::move(error);
    }
    // 释放异常锁后再进入队列，避免同时持有两把锁；后续异常不覆盖首个。
    queue_.cancel();
}

void Failure::rethrow() {
    // 由主线程在 join 所有已启动线程后调用，恢复原异常类型及诊断信息。
    std::exception_ptr error; // 在锁内取得的异常副本，锁外重新抛出。
    { std::lock_guard<std::mutex> lock(mutex_); error = first_; }
    if (error) std::rethrow_exception(error);
}

} // namespace flash_csv
