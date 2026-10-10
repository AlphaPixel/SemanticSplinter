// Runs a program without a shell (SPEC §9 exceptions: `git config`, git-setup, --sign-command).
#include "cli.hpp"

#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace splinter::cli {

#ifdef _WIN32

namespace {

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The quoting that CommandLineToArgvW and the MSVC runtime undo.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return a;
    std::wstring q = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            q.append(slashes * 2 + 1, L'\\');
            slashes = 0;
        } else {
            slashes = 0;
        }
        q += c;
    }
    q.append(slashes, L'\\');
    return q + L"\"";
}

} // namespace

std::optional<ProcessResult> run_process(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (auto& a : argv)
        cmd += (cmd.empty() ? L"" : L" ") + quote(wide(a));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return std::nullopt;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return std::nullopt;
    }
    ProcessResult r;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n)
        r.out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    r.status = static_cast<int>(code);
    return r;
}

#else

std::optional<ProcessResult> run_process(const std::vector<std::string>& argv) {
    int fd[2];
    if (argv.empty() || pipe(fd) != 0)
        return std::nullopt;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    posix_spawn_file_actions_addclose(&fa, fd[1]);
    std::vector<char*> args;
    for (auto& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (rc != 0) {
        close(fd[0]);
        return std::nullopt;
    }
    ProcessResult r;
    char buf[4096];
    for (ssize_t n; (n = read(fd[0], buf, sizeof buf)) > 0;)
        r.out.append(buf, static_cast<std::size_t>(n));
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    return r;
}

#endif

} // namespace splinter::cli
