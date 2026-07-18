#pragma once

#include "minidb/common/result.hpp"
#include "minidb/common/sql_parser.hpp"

#include <functional>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace minidb {

// 五个命令行程序共用的逐行 REPL。execute 回调决定命令进入哪一种数据库。
inline int run_repl(const std::string& banner,
                    const std::string& prompt,
                    const std::function<QueryResult(const std::string&)>& execute) {
#ifdef _WIN32
    // Windows 终端默认代码页不一定是 UTF-8，主动设置后中文提示才能稳定显示。
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << banner << "\n输入 HELP 查看语法，输入 QUIT 退出。\n\n";
    std::string line;
    while (true) {
        std::cout << prompt << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        // QUIT 由 REPL 本身消费，避免数据库执行器承担终端生命周期管理。
        const auto parsed = parse_sql(line);
        if (parsed.ok() && std::holds_alternative<QuitStatement>(*parsed.statement)) {
            break;
        }
        std::cout << execute(line).format() << "\n";
    }
    return 0;
}

}  // namespace minidb
