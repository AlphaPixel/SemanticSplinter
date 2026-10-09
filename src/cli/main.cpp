#include "cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace {

class StdConsole final : public splinter::cli::Console {
public:
    void out(std::string_view s) override { std::fwrite(s.data(), 1, s.size(), stdout); }
    void err(std::string_view s) override { std::fwrite(s.data(), 1, s.size(), stderr); }
    bool out_is_terminal() const override { return isatty(fileno(stdout)) != 0; }
    std::optional<std::string> env(const char* name) const override {
#ifdef _WIN32
        std::wstring wname(name, name + std::strlen(name));
        DWORD len = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
        if (!len)
            return std::nullopt;
        std::wstring w(len, L'\0');
        w.resize(GetEnvironmentVariableW(wname.c_str(), w.data(), len));
        const wchar_t* v = w.c_str();
        int n = WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, v, -1, s.data(), n, nullptr, nullptr);
        return s;
#else
        const char* v = std::getenv(name);
        return v ? std::optional<std::string>(v) : std::nullopt;
#endif
    }
    std::vector<std::byte> read_stdin() override {
        std::vector<std::byte> data;
        std::byte buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, stdin)) > 0;)
            data.insert(data.end(), buf, buf + n);
        return data;
    }
};

} // namespace

#ifdef _WIN32
// Arguments are converted to UTF-8 at the edge (D-24). I/O is binary so LF stays LF, as GNU tools write it.
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    StdConsole c;
    int rc = splinter::cli::run(std::move(args), c);
    std::fflush(stdout);
    return rc;
}
#else
int main(int argc, char** argv) {
    StdConsole c;
    int rc = splinter::cli::run(std::vector<std::string>(argv, argv + argc), c);
    std::fflush(stdout);
    return rc;
}
#endif
