#include <semsplinter/version.hpp>

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

int run(const std::vector<std::string>& args) {
    if (args.size() == 2 && (args[1] == "--version" || args[1] == "-v" || args[1] == "version")) {
        std::printf("splinter (Semantic Splinter) %.*s\n", static_cast<int>(semsplinter::version().size()),
                    semsplinter::version().data());
        return 0;
    }
    std::fputs("splinter: nothing implemented yet; try 'splinter --version'\n", stderr);
    return 2;
}

} // namespace

#ifdef _WIN32
// Arguments are converted to UTF-8 at the edge (D-24). Output is binary so LF stays LF, as GNU tools write
// it.
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
