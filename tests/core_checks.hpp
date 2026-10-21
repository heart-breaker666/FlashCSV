#pragma once
#include "flash_csv/types.hpp"
#include <functional>
#include <iostream>

inline void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

inline void fails(const std::function<void()>& action, const std::string& kind) {
    try { action(); }
    catch (const flash_csv::Error& error) {
        check(std::string(error.what()).find(kind) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("expected failure: " + kind);
}
