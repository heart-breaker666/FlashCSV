#include "flash_csv/engine.hpp"
#include "flash_csv/query.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    // 入口只组织参数、帮助、执行和报告，业务处理由各模块完成。
    try {
        const auto config = flash_csv::parse_arguments(argc, argv); // 命令行解析出的执行要求。
        if (config.help) {
            std::cout << flash_csv::usage();
            return 0;
        }
        const auto stats = flash_csv::execute(config); // 成功发布结果后返回的运行统计。
        // 统计写到 stderr，CSV 结果由 execute 写入指定文件。
        std::cerr << flash_csv::report(stats) << '\n';
        return 0;
    } catch (const flash_csv::Error& error) {
        // 项目异常保留约定的退出码；其他异常在后续分支统一视为运行失败。
        std::cerr << "error: " << error.what() << '\n';
        return error.exit_code();
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "error: unknown runtime failure\n";
        return 1;
    }
}
