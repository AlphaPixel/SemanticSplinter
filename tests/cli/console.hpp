#pragma once

#include "cli.hpp"

#include <map>
#include <string>
#include <vector>

// Captures what a command writes, for in-process CLI tests.
struct CaptureConsole final : splinter::cli::Console {
    std::string out_text, err_text;
    std::map<std::string, std::string> vars;
    std::vector<std::byte> input;
    bool terminal = false; // stdout is a terminal
    void out(std::string_view s) override { out_text += s; }
    void err(std::string_view s) override { err_text += s; }
    std::optional<std::string> env(const char* name) const override {
        auto it = vars.find(name);
        return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::vector<std::byte> read_stdin() override { return input; }
    bool out_is_terminal() const override { return terminal; }
};

inline int run_cli(CaptureConsole& c, std::vector<std::string> args) {
    args.insert(args.begin(), "splinter");
    return splinter::cli::run(std::move(args), c);
}
