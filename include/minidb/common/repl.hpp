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

inline int run_repl(const std::string& banner,
                    const std::string& prompt,
                    const std::function<QueryResult(const std::string&)>& execute) {
#ifdef _WIN32
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
        const auto parsed = parse_sql(line);
        if (parsed.ok() && std::holds_alternative<QuitStatement>(*parsed.statement)) {
            break;
        }
        std::cout << execute(line).format() << "\n";
    }
    return 0;
}

}  // namespace minidb
