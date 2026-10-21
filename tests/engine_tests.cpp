#include "flash_csv/engine.hpp"
#include "core_checks.hpp"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace flash_csv;
namespace fs = std::filesystem;

std::size_t thread_count() {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/task"), fs::directory_iterator{}));
}

std::size_t fd_count() {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}

bool threads_reclaimed(std::size_t baseline) {
    // pthread_join can return just before the kernel removes /proc task entries.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (thread_count() != baseline) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc != 2) return 2;
    const fs::path directory = fs::path(argv[1]) / ("engine-" + std::to_string(::getpid()));
    fs::create_directories(directory);
    try {
        const auto baseline = thread_count();
        const auto descriptors = fd_count();
        Config config;
        config.mode = Mode::Aggregate;
        config.input = (directory / "input.csv").string();
        config.output = (directory / "output.csv").string();
        config.threads = 4;
        config.limits.batch_rows = 1;
        config.limits.queue_batches = 1;
        config.aggregates = {{Function::Count, ""}};
        { std::ofstream input(config.input); input << "n\n1\n2\n3\n"; }
        ExecutionHooks hooks;
        hooks.before_worker = [](std::size_t i) {
            if (i == 2) throw Error(1, "start", "injected partial thread startup failure");
        };
        fails([&] { execute(config, hooks); }, "start");
        check(threads_reclaimed(baseline) && fd_count() == descriptors && !fs::exists(config.output),
              "startup reclaims threads and descriptors");
        for (const auto& stage : {"write", "flush", "close", "publish"}) {
            hooks = {};
            hooks.output_checkpoint = [stage](const char* point) {
                if (std::string(point) == stage) throw Error(1, "io", "injected output failure");
            };
            fails([&] { execute(config, hooks); }, "io");
            check(threads_reclaimed(baseline) && fd_count() == descriptors && !fs::exists(config.output),
                  "output failure cleanup and resources");
            for (const auto& item : fs::directory_iterator(directory))
                check(item.path().filename().string().rfind(".flash_csv-", 0) != 0, "temporary cleanup");
        }
        hooks = {};
        hooks.output_checkpoint = [&](const char* point) {
            if (std::string(point) == "publish") { std::ofstream existing(config.output); existing << "sentinel\n"; }
        };
        fails([&] { execute(config, hooks); }, "publish");
        std::ifstream existing(config.output);
        std::string line;
        std::getline(existing, line);
        existing.close();
        check(line == "sentinel", "publication race preserves destination");
        fs::remove(config.output);
        hooks.output_checkpoint = [](const char* point) {
            if (std::string(point) == "cleanup") throw Error(1, "cleanup", "injected removal failure");
        };
        const auto stats = execute(config, hooks);
        check(stats.read_rows == 3 && stats.matched_rows == 3 && stats.output_rows == 1, "successful stats");
        check(fs::exists(config.output) && threads_reclaimed(baseline) && fd_count() == descriptors,
              "cleanup warning preserves published output and releases resources");
        fs::remove_all(directory);
        std::cout << "Engine failure and resource tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(directory);
        return 1;
    }
}
