module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

module ngine.platform;

import ngine.core;

namespace ngine::platform {

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<usize>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size);
    return result;
}

// Ein Argument nach den Regeln von CommandLineToArgvW.
void append_quoted(std::wstring& line, const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    for (auto it = argument.begin();; ++it) {
        usize backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        line += *it;
    }
    line += L'"';
}

std::string last_error() {
    return std::format("Fehler {}", GetLastError());
}

} // namespace

std::expected<ProcessResult, std::string> run_process(std::string_view program,
                                                      std::span<const std::string> arguments,
                                                      std::string_view working_directory) {
    SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                 .lpSecurityDescriptor = nullptr,
                                 .bInheritHandle = TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &security, 0) == FALSE) {
        return std::unexpected(std::format("Pipe für {} fehlt: {}", program, last_error()));
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, 0, nullptr);

    std::wstring line;
    append_quoted(line, widen(program));
    for (const std::string& argument : arguments) {
        line += L' ';
        append_quoted(line, widen(argument));
    }
    const std::wstring directory = widen(working_directory);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION process{};
    const BOOL created =
        CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       directory.empty() ? nullptr : directory.c_str(), &startup, &process);
    CloseHandle(write_end);
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (created == FALSE) {
        const std::string error = last_error();
        CloseHandle(read_end);
        return std::unexpected(std::format("{} lässt sich nicht starten: {}", program, error));
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) !=
               FALSE &&
           count > 0) {
        result.output.append(buffer.data(), count);
    }
    CloseHandle(read_end);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<i32>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

} // namespace ngine::platform
