#include "flash_csv/csv.hpp"
#include "flash_csv/query.hpp"
#include "flash_csv/aggregate.hpp"
#include "flash_csv/pipeline.hpp"
#include "core_checks.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <future>
#include <limits>
#include <sstream>

using namespace flash_csv;

void csv_tests() {
    std::istringstream input("\xef\xbb\xbf" "a,b\r\n\"x,y\",\"a\"\"b\"\r\n,\nlast,z");
    CsvReader reader(input, 12);
    check(reader.header() == std::vector<std::string>({"a", "b"}), "BOM header");
    Row row;
    check(reader.next(row) && row.line == 2 && row.fields[1] == "a\"b", "quoted row");
    check(reader.next(row) && row.fields[0].empty() && row.line == 3, "empty fields");
    check(reader.next(row) && row.line == 4 && !reader.next(row), "EOF row");
    std::ostringstream output;
    write_record(output, {"x,y", "a\"b", "", " plain "});
    check(output.str() == "\"x,y\",\"a\"\"b\",, plain \n", "CSV escaping");
    for (const auto& text : {"a\n\"x\n", "a\nx\"\n", "a\n\"x\"z\n", "a\n\"x", "a\rx"}) {
        fails([&] { std::istringstream in(text); CsvReader r(in, 64); r.next(row); },
              text == std::string("a\n\"x\n") ? "unsupported_multiline" : "csv_syntax");
    }
    fails([] { std::istringstream in(""); CsvReader r(in, 64); }, "missing_header");
    fails([] { std::istringstream in("a,a\n"); CsvReader r(in, 64); }, "duplicate_header");
    fails([] { std::istringstream in("a,\n"); CsvReader r(in, 64); }, "empty_header");
    fails([&] { std::istringstream in("a,b\nx\n"); CsvReader r(in, 64); r.next(row); }, "field_count");
    for (const auto& ending : {"", "\n", "\r\n"}) {
        std::istringstream in("a\n1234" + std::string(ending));
        CsvReader r(in, 4);
        check(r.next(row) && row.fields[0] == "1234", "exact record limit");
        fails([&] { std::istringstream over("a\n12345" + std::string(ending));
                    CsvReader limited(over, 4); limited.next(row); }, "record_limit");
    }
    std::istringstream blank("a\n\n");
    CsvReader r(blank, 1);
    check(r.next(row) && row.fields == std::vector<std::string>({""}) && !r.next(row), "blank record");
    std::istringstream broken("a\n");
    broken.setstate(std::ios::badbit);
    fails([&] { CsvReader bad(broken, 64); }, "io");
}

void query_tests() {
    for (const auto& text : {"0", "-0", "1.", ".5", "-.5", "2e+3", "-2E-3"})
        check(std::isfinite(parse_number(text, 2, "n")), "valid number");
    for (const auto& text : {"", "+1", " 1", "1 ", "0x1", "NaN", "Infinity", ".", "1e", "1e999", "1e-999", "1,2"})
        fails([&] { parse_number(text, 2, "n"); }, "invalid_number");
    Config config;
    config.select = {"n", "key"};
    config.filters = {{"key", false, Compare::Eq, "yes"}, {"n", true, Compare::Ge, "1"}};
    Query query(config, {"key", "n"});
    check(!query.matches(Row{2, {"no", "bad"}}), "short circuit");
    check(query.matches(Row{3, {"yes", "2"}}), "numeric predicate");
    check(query.project(Row{3, {"yes", "2"}}) == std::vector<std::string>({"2", "yes"}), "select order");
    fails([&] { query.matches(Row{4, {"yes", "bad"}}); }, "line 4 column 'n'");
    config.select = {"missing"};
    fails([&] { Query invalid(config, {"key", "n"}); }, "unknown column");
    config.select = {"n", "n"};
    fails([&] { Query invalid(config, {"key", "n"}); }, "duplicate");
}

void aggregation_tests() {
    Config config;
    config.mode = Mode::Aggregate;
    config.group_by = "key";
    config.aggregates = {{Function::Count, ""}, {Function::Sum, "a"},
                         {Function::Min, "b"}, {Function::Max, "b"}, {Function::Avg, "a"}};
    Query query(config, {"key", "a", "b"});
    Aggregator first(query, 2), second(query, 2);
    first.consume(Row{2, {"x", "100", "-3"}});
    second.consume(Row{3, {"x", "200", "8"}});
    second.consume(Row{4, {"x", "300", "5"}});
    first.merge(second);
    std::ostringstream result;
    first.write(result);
    check(result.str() == "x,3,600,-3,8,200\n", "weighted AVG and different columns");
    Aggregator other(query, 2);
    other.consume(Row{5, {"y", "1", "2"}});
    other.consume(Row{6, {"z", "1", "2"}});
    fails([&] { first.merge(other); }, "group_limit");
    Aggregator local(query, 1);
    local.consume(Row{2, {"x", "1", "2"}});
    fails([&] { local.consume(Row{3, {"y", "1", "2"}}); }, "group_limit line 3 column 'key'");
    GroupState target{std::numeric_limits<std::uint64_t>::max(), std::vector<double>(5, 0)};
    GroupState source{1, std::vector<double>(5, 0)};
    fails([&] { merge_state(target, source, query); }, "count_overflow");
    target = GroupState{1, std::vector<double>(5, 1e308)};
    source = target;
    fails([&] { merge_state(target, source, query); }, "sum_overflow");
    Aggregator sums(query, 1);
    sums.consume(Row{2, {"x", "1e308", "0"}});
    fails([&] { sums.consume(Row{3, {"x", "1e308", "0"}}); }, "sum_overflow line 3 column 'a'");
    config.aggregates = {{Function::Count, ""}};
    Query count(config, {"key", "a", "b"});
    Aggregator sorted(count, 4);
    for (const auto& key : {std::string("\xff"), std::string("z"), std::string(""), std::string("\x80")})
        sorted.consume(Row{2, {key, "bad", "bad"}});
    result.str("");
    sorted.write(result);
    check(result.str() == ",1\nz,1\n\x80,1\n\xff,1\n", "unsigned byte ordering");
}

void batch_tests() {
    Limits limits;
    limits.max_record_bytes = 4;
    limits.batch_bytes = 4;
    limits.batch_rows = 10;
    std::istringstream input("a\n123\n45\n67\n\n");
    CsvReader reader(input, 4);
    BatchReader batches(reader, limits);
    Batch batch;
    check(batches.next(batch) && batch.bytes == 3 && batch.rows.size() == 1, "pending record boundary");
    check(batches.next(batch) && batch.bytes == 4 && batch.rows.size() == 2, "exact batch byte boundary");
    check(batches.next(batch) && batch.bytes == 0 && batch.rows.size() == 1, "zero byte row");
    check(!batches.next(batch) && batches.rows_read() == 4, "batch read count");
    limits.batch_rows = 2;
    std::istringstream rows("a\n\n\n\n");
    CsvReader empty(rows, 4);
    BatchReader row_batches(empty, limits);
    check(row_batches.next(batch) && batch.rows.size() == 2, "batch row boundary");
    check(row_batches.next(batch) && batch.rows.size() == 1 && !row_batches.next(batch), "row limit remainder");
}

void queue_tests() {
    using namespace std::chrono_literals;
    BatchQueue normal(2);
    check(normal.push(Batch{{Row{2, {"x"}}}, 1}), "queue push");
    normal.close();
    Batch batch;
    check(normal.pop(batch) && batch.rows[0].line == 2 && !normal.pop(batch), "close drains");
    check(!normal.push(Batch{}), "closed queue rejects push");
    BatchQueue full(1);
    full.push(Batch{});
    auto writer = std::async(std::launch::async, [&] { return full.push(Batch{}); });
    check(writer.wait_for(30ms) == std::future_status::timeout, "producer blocks on full queue");
    full.cancel();
    check(writer.wait_for(1s) == std::future_status::ready && !writer.get(), "cancel wakes producer");
    check(!full.pop(batch), "cancel drops queued batch");
    BatchQueue empty(1);
    auto reader = std::async(std::launch::async, [&] { Batch b; return empty.pop(b); });
    check(reader.wait_for(30ms) == std::future_status::timeout, "consumer blocks on empty queue");
    empty.cancel();
    check(reader.wait_for(1s) == std::future_status::ready && !reader.get(), "cancel wakes consumer");
    Failure failure(empty);
    failure.capture(std::make_exception_ptr(Error(1, "first", "first failure")));
    failure.capture(std::make_exception_ptr(Error(1, "second", "second failure")));
    fails([&] { failure.rethrow(); }, "first");
}

int main() {
    try {
        csv_tests();
        query_tests();
        aggregation_tests();
        batch_tests();
        queue_tests();
        std::cout << "CSV, query, aggregate, batch and queue tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
