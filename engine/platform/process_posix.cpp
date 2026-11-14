module;

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module ngine.platform;

import ngine.core;

namespace ngine::platform {

std::expected<ProcessResult, std::string> run_process(std::string_view program,
                                                      std::span<const std::string> arguments,
                                                      std::string_view working_directory) {
    std::array<int, 2> pipe_fds{-1, -1};
    if (pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
        return std::unexpected(std::format("Pipe für {} fehlt: {}", program, std::strerror(errno)));
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    const std::string directory{working_directory};
    if (!directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    }

    const std::string name{program};
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(name.c_str()));
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, name.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (spawned != 0) {
        close(pipe_fds[0]);
        return std::unexpected(
            std::format("{} lässt sich nicht starten: {}", program, std::strerror(spawned)));
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0) {
            result.output.append(buffer.data(), static_cast<usize>(count));
        } else if (count == 0 || errno != EINTR) {
            break;
        }
    }
    close(pipe_fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

} // namespace ngine::platform
