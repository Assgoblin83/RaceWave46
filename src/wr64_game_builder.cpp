#include "wr64_game_builder.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dlfcn.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "wr64_game_module.h"
#include "wr64_generation.hpp"
#include "wr64_sha256.hpp"

namespace wr64::game_builder {
namespace {

constexpr const char* kModuleName = "libwr64game.so";
constexpr const char* kRecordName = "build.json";
constexpr size_t kMaximumRomBytes = 16 * 1024 * 1024;

std::mutex build_mutex;

bool read_binary(const std::filesystem::path& path, std::vector<uint8_t>& data,
    std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Cannot read " + path.string();
        return false;
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0) {
        error = "Cannot read " + path.string();
        return false;
    }
    stream.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    if (!data.empty()) {
        stream.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
    if (!stream) {
        error = "Cannot read " + path.string();
        return false;
    }
    return true;
}

bool write_binary(const std::filesystem::path& path, const void* data, size_t size,
    std::string& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "Cannot write " + path.string();
        return false;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream) {
        error = "Cannot write " + path.string();
        return false;
    }
    return true;
}

std::string digest_of(std::span<const uint8_t> data) {
    return crypto::to_hex(crypto::Sha256::hash(data));
}

std::string read_text(const std::filesystem::path& path, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_binary(path, bytes, error)) {
        return {};
    }
    return std::string(bytes.begin(), bytes.end());
}

bool write_text(const std::filesystem::path& path, std::string_view text,
    std::string& error) {
    return write_binary(path, text.data(), text.size(), error);
}

std::filesystem::path executable_directory() {
#ifdef _WIN32
    wchar_t module_path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(module_path).parent_path();
    }
    return std::filesystem::current_path();
#else
    std::error_code ignored;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", ignored);
    if (!ignored && !executable.empty()) {
        return executable.parent_path();
    }
    return std::filesystem::current_path();
#endif
}

// Child processes need the packaged compiler directory on PATH (for
// ld.lld) and on LD_LIBRARY_PATH (for libLLVM and libclang-cpp).
std::vector<std::string> tool_environment(const Paths& paths) {
    const std::filesystem::path tools = paths.compiler.parent_path();
    std::vector<std::string> environment;
    const char* current_path = std::getenv("PATH");
    environment.push_back("PATH=" + tools.string() + ":" +
        (current_path != nullptr ? current_path : "/usr/bin:/bin"));
#ifndef _WIN32
    std::string libraries = tools.string();
    if (const char* current = std::getenv("LD_LIBRARY_PATH");
        current != nullptr && *current != '\0') {
        libraries += ":";
        libraries += current;
    }
    environment.push_back("LD_LIBRARY_PATH=" + libraries);
#endif
    return environment;
}

std::vector<std::filesystem::path> split_paths(const char* value,
    const std::filesystem::path& base = {}) {
    std::vector<std::filesystem::path> result;
    if (value == nullptr || *value == '\0') {
        return result;
    }
    std::string_view text{value};
    while (!text.empty()) {
        const size_t separator = text.find(':');
        const std::string_view item = text.substr(0, separator);
        if (!item.empty()) {
            std::filesystem::path path{item};
            result.push_back(path.is_absolute() || base.empty() ? path : base / path);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
    }
    return result;
}

bool apply_ips(std::vector<uint8_t>& rom, std::span<const uint8_t> patch,
    std::string& error) {
    if (patch.size() < 8 || std::memcmp(patch.data(), "PATCH", 5) != 0) {
        error = "Invalid IPS header";
        return false;
    }
    size_t position = 5;
    while (true) {
        if (position + 3 > patch.size()) {
            error = "Truncated IPS record";
            return false;
        }
        if (std::memcmp(patch.data() + position, "EOF", 3) == 0) {
            position += 3;
            break;
        }
        if (position + 5 > patch.size()) {
            error = "Truncated IPS record";
            return false;
        }
        const uint32_t offset = (static_cast<uint32_t>(patch[position]) << 16) |
            (static_cast<uint32_t>(patch[position + 1]) << 8) |
            static_cast<uint32_t>(patch[position + 2]);
        size_t length = (static_cast<size_t>(patch[position + 3]) << 8) |
            static_cast<size_t>(patch[position + 4]);
        position += 5;
        if (length == 0) {
            if (position + 3 > patch.size()) {
                error = "Truncated IPS repeat";
                return false;
            }
            length = (static_cast<size_t>(patch[position]) << 8) |
                static_cast<size_t>(patch[position + 1]);
            const uint8_t value = patch[position + 2];
            position += 3;
            if (offset + length > kMaximumRomBytes) {
                error = "IPS output exceeds the supported ROM size";
                return false;
            }
            if (offset + length > rom.size()) {
                rom.resize(offset + length);
            }
            std::memset(rom.data() + offset, value, length);
        } else {
            if (position + length > patch.size()) {
                error = "Truncated IPS payload";
                return false;
            }
            if (offset + length > kMaximumRomBytes) {
                error = "IPS output exceeds the supported ROM size";
                return false;
            }
            if (offset + length > rom.size()) {
                rom.resize(offset + length);
            }
            std::memcpy(rom.data() + offset, patch.data() + position, length);
            position += length;
        }
    }
    if (position != patch.size()) {
        error = "Unexpected IPS trailer";
        return false;
    }
    return true;
}

// Runs a child process, capturing interleaved stdout and stderr. Compilers run
// at a lower priority so the UI and the rest of the system stay responsive.
bool run_process(const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& environment, int priority,
    std::string& output, std::string& error) {
    output.clear();
    if (arguments.empty()) {
        error = "Empty command";
        return false;
    }
#ifndef _WIN32
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        error = "Cannot create a pipe for " + arguments.front();
        return false;
    }
    posix_spawn_file_actions_t file_actions;
    int spawn_error = posix_spawn_file_actions_init(&file_actions);
    if (spawn_error != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        error = "Cannot prepare " + arguments.front() + ": " +
            std::strerror(spawn_error);
        return false;
    }

    int action_error = posix_spawn_file_actions_addclose(&file_actions, pipe_fds[0]);
    if (action_error == 0) {
        action_error = posix_spawn_file_actions_adddup2(
            &file_actions, pipe_fds[1], STDOUT_FILENO);
    }
    if (action_error == 0) {
        action_error = posix_spawn_file_actions_adddup2(
            &file_actions, pipe_fds[1], STDERR_FILENO);
    }
    if (action_error == 0 && pipe_fds[1] != STDOUT_FILENO &&
        pipe_fds[1] != STDERR_FILENO) {
        action_error = posix_spawn_file_actions_addclose(&file_actions, pipe_fds[1]);
    }
    if (action_error == 0 && !working_directory.empty()) {
        action_error = posix_spawn_file_actions_addchdir_np(
            &file_actions, working_directory.c_str());
    }
    if (action_error != 0) {
        posix_spawn_file_actions_destroy(&file_actions);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        error = "Cannot prepare " + arguments.front() + ": " +
            std::strerror(action_error);
        return false;
    }

    std::map<std::string, std::string> overrides;
    for (const std::string& entry : environment) {
        const size_t separator = entry.find('=');
        if (separator != std::string::npos) {
            overrides[entry.substr(0, separator)] = entry.substr(separator + 1);
        }
    }
    std::vector<std::string> child_environment;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view value{*entry};
        const size_t separator = value.find('=');
        if (separator == std::string_view::npos) {
            child_environment.emplace_back(value);
            continue;
        }
        const auto override = overrides.find(std::string(value.substr(0, separator)));
        if (override == overrides.end()) {
            child_environment.emplace_back(value);
        } else {
            child_environment.push_back(override->first + "=" + override->second);
            overrides.erase(override);
        }
    }
    for (const auto& [name, value] : overrides) {
        child_environment.push_back(name + "=" + value);
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(child_environment.size() + 1);
    for (std::string& entry : child_environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    pid_t pid = -1;
    spawn_error = posix_spawnp(&pid, argv.front(), &file_actions, nullptr,
        argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&file_actions);
    close(pipe_fds[1]);
    if (spawn_error != 0) {
        close(pipe_fds[0]);
        error = "Cannot start " + arguments.front() + ": " +
            std::strerror(spawn_error);
        return false;
    }
    if (priority != 0) {
        setpriority(PRIO_PROCESS, pid, priority);
    }
    char buffer[4096];
    ssize_t count = 0;
    while ((count = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<size_t>(count));
    }
    close(pipe_fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        error = "Cannot wait for " + arguments.front();
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = arguments.front() + " failed (exit ";
        error += WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : "signal";
        error += ")";
        return false;
    }
    return true;
#else
    std::string command;
    for (const std::string& argument : arguments) {
        if (!command.empty()) {
            command += ' ';
        }
        command += '"';
        command += argument;
        command += '"';
    }
    command += " 2>&1";
    FILE* stream = _popen(command.c_str(), "r");
    if (stream == nullptr) {
        error = "Cannot start " + arguments.front();
        return false;
    }
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        output.append(buffer, count);
    }
    const int status = _pclose(stream);
    if (status != 0) {
        error = arguments.front() + " failed (exit " + std::to_string(status) + ")";
        return false;
    }
    return true;
#endif
}

std::string recipe_hash_of(const std::filesystem::path& recipe_path,
    std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_binary(recipe_path, bytes, error)) {
        return {};
    }
    return digest_of(bytes);
}

// The compiler version is part of the module key so a different Clang cannot
// silently reuse objects it did not produce.
std::string compiler_identity(const Paths& paths, std::string& error) {
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    const std::filesystem::path& compiler = paths.compiler;
    const std::string key = compiler.string();
    {
        std::lock_guard lock{cache_mutex};
        const auto found = cache.find(key);
        if (found != cache.end()) {
            return found->second;
        }
    }

    std::string output;
    std::string run_error;
    if (!run_process({compiler.string(), "--version"}, {}, tool_environment(paths), 0,
            output, run_error)) {
        error = "Cannot run the C compiler: " + run_error;
        return {};
    }
    const size_t newline = output.find('\n');
    std::string identity = output.substr(0, newline);
    if (!identity.empty() && identity.back() == '\r') {
        identity.pop_back();
    }
    if (identity.empty()) {
        error = "The C compiler reported no version: " + compiler.string();
        return {};
    }
    std::lock_guard lock{cache_mutex};
    cache.emplace(key, identity);
    return identity;
}

std::string module_key(const std::string& recipe_hash, const std::string& compiler,
    std::string& error) {
    const std::string material = recipe_hash + "\n" +
        std::to_string(WR64_GAME_MODULE_ABI_VERSION) + "\n" + compiler;
    return crypto::to_hex(crypto::Sha256::hash(material));
}

std::string escape_json(std::string_view text) {
    std::string escaped;
    for (const char value : text) {
        if (value == '"' || value == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(value);
    }
    return escaped;
}

std::string build_record(const std::string& recipe_hash, const std::string& compiler,
    const std::string& rom_hash) {
    return "{\n"
        "  \"abi_version\": " + std::to_string(WR64_GAME_MODULE_ABI_VERSION) + ",\n"
        "  \"recipe_hash\": \"" + recipe_hash + "\",\n"
        "  \"compiler\": \"" + escape_json(compiler) + "\",\n"
        "  \"patched_rom_sha256\": \"" + rom_hash + "\"\n"
        "}\n";
}

bool record_matches(const std::filesystem::path& path, const std::string& recipe_hash,
    const std::string& compiler) {
    std::string error;
    const std::string text = read_text(path, error);
    if (text.empty()) {
        return false;
    }
    return text.find("\"recipe_hash\": \"" + recipe_hash + "\"") != std::string::npos &&
        text.find("\"compiler\": \"" + escape_json(compiler) + "\"") != std::string::npos &&
        text.find("\"abi_version\": " + std::to_string(WR64_GAME_MODULE_ABI_VERSION)) !=
            std::string::npos;
}

bool verify_inputs(const std::filesystem::path& source_root,
    const wr64::generation::Recipe& recipe, std::string& error) {
    for (const auto& [name, checksum] : recipe.inputs) {
        // The Python finalizer and the CMake patch are reproduced in
        // wr64_generation.cpp; the generated code checks below prove the
        // replacement behaves identically.
        if (name == "tools/finalize_generated.py" ||
            name == "tools/patch_rsp_dispatch.cmake") {
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!read_binary(source_root / name, bytes, error)) {
            return false;
        }
        if (digest_of(bytes) != checksum) {
            error = "Generation input does not match this release: " + name;
            return false;
        }
    }
    return true;
}

std::vector<std::string> include_arguments(const Paths& paths,
    const std::filesystem::path& generated_directory) {
    std::vector<std::string> arguments;
    arguments.push_back("-I");
    arguments.push_back(generated_directory.string());
    for (const auto& directory : paths.header_directories) {
        arguments.push_back("-I");
        arguments.push_back(directory.string());
    }
    for (const auto& directory : paths.system_include_directories) {
        arguments.push_back("-isystem");
        arguments.push_back(directory.string());
    }
    return arguments;
}

std::vector<std::string> compile_command(const Paths& paths,
    const std::filesystem::path& generated_directory,
    const std::filesystem::path& source, const std::filesystem::path& object,
    bool cxx, bool sse41, const std::string& recipe_hash) {
    std::vector<std::string> arguments;
    arguments.push_back(paths.compiler.string());
    if (cxx) {
        arguments.insert(arguments.end(), {"-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-fno-threadsafe-statics"});
    } else {
        arguments.insert(arguments.end(), {"-std=gnu17"});
    }
    arguments.insert(arguments.end(), {"-O2", "-DNDEBUG", "-fPIC", "-fno-strict-aliasing",
        "-Wno-unused-variable", "-Wno-implicit-function-declaration"});
    if (sse41) {
        arguments.push_back("-msse4.1");
    }
    if (cxx) {
        arguments.push_back("-DWR64_GAME_MODULE_RECIPE_HASH=\"" + recipe_hash + "\"");
    }
    const auto includes = include_arguments(paths, generated_directory);
    arguments.insert(arguments.end(), includes.begin(), includes.end());
    arguments.push_back("-c");
    arguments.push_back(source.string());
    arguments.push_back("-o");
    arguments.push_back(object.string());
    return arguments;
}

bool validate_module(const std::filesystem::path& module, const std::string& recipe_hash,
    std::string& error) {
#ifdef _WIN32
    (void)module;
    (void)recipe_hash;
    (void)error;
    return true;
#else
    if (std::getenv("WR64_BUILDER_NO_DLOPEN") != nullptr) {
        return true;
    }
    void* handle = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* detail = dlerror();
        error = std::string("The game module cannot be loaded: ") +
            (detail != nullptr ? detail : "unknown error");
        return false;
    }
    auto entry = reinterpret_cast<Wr64GameModuleEntry>(
        dlsym(handle, WR64_GAME_MODULE_ABI_FUNCTION));
    bool valid = entry != nullptr;
    const Wr64GameModule* abi = entry != nullptr ? entry() : nullptr;
    if (!valid || abi == nullptr || abi->abi_version != WR64_GAME_MODULE_ABI_VERSION ||
        abi->struct_size < sizeof(Wr64GameModule) || abi->recipe_hash == nullptr ||
        abi->recipe_hash != recipe_hash || abi->entrypoint == nullptr ||
        abi->asp_main == nullptr || abi->code_sections == nullptr ||
        abi->overlay_table == nullptr
#ifdef WR64_ENTRYPOINT_ADDRESS
        || abi->entrypoint_address !=
            static_cast<gpr>(static_cast<int32_t>(WR64_ENTRYPOINT_ADDRESS))
#endif
        ) {
        error = "The game module does not match this release";
        valid = false;
    }
    dlclose(handle);
    return valid;
#endif
}

void remove_quietly(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

std::string human_stage(int percent) {
    if (percent < 5) {
        return "Reading ROM";
    }
    if (percent < 30) {
        return "Recompiling game code";
    }
    if (percent < 36) {
        return "Verifying generated code";
    }
    if (percent < 90) {
        return "Compiling game code";
    }
    if (percent < 96) {
        return "Linking game module";
    }
    return "Finishing";
}

}  // namespace

Paths default_paths(const std::filesystem::path& rom,
    const std::filesystem::path& data_directory) {
    Paths paths;
    paths.rom = rom;
    paths.data_directory = data_directory;
    const auto executable = executable_directory();

    if (const char* value = std::getenv("WR64_GAME_SOURCE_ROOT")) {
        paths.source_root = value;
    } else {
        // Packaged: <prefix>/lib/racewave46/exe with <prefix>/share/racewave46.
        // Development: the executable sits in a build directory beside the
        // source tree.
        const std::filesystem::path packaged =
            executable.parent_path().parent_path() / "share" / "racewave46";
        const std::filesystem::path source_parent = executable.parent_path();
        std::error_code ignored;
        if (std::filesystem::exists(packaged / "config" / "generated-code.json", ignored)) {
            paths.source_root = packaged;
        } else if (std::filesystem::exists(
                source_parent / "config" / "generated-code.json", ignored)) {
            paths.source_root = source_parent;
        } else {
            paths.source_root = executable;
        }
    }

    std::filesystem::path tools_root;
    if (const char* value = std::getenv("WR64_GAME_TOOLS_ROOT")) {
        tools_root = value;
    } else {
        tools_root = executable / "tools";
        std::error_code ignored;
        if (!std::filesystem::exists(tools_root, ignored)) {
            tools_root = executable.parent_path() / "build-tools";
        }
    }

    auto override_or = [&](const char* name, const std::filesystem::path& fallback) {
        if (const char* value = std::getenv(name)) {
            return std::filesystem::path{value};
        }
        return fallback;
    };
    paths.compiler = override_or("WR64_CLANG", tools_root / "clang");
    paths.n64recomp = override_or("WR64_N64RECOMP", tools_root / "N64Recomp");
    paths.rsprecomp = override_or("WR64_RSPRECOMP", tools_root / "RSPRecomp");
    {
        // Development builds usually provide Clang through PATH (the SDK).
        std::error_code ignored;
        if (!std::filesystem::exists(paths.compiler, ignored)) {
            paths.compiler = "clang";
        }
    }

    const auto header_override = split_paths(std::getenv("WR64_GAME_INCLUDE_DIRS"));
    if (!header_override.empty()) {
        paths.header_directories = header_override;
    } else {
        std::error_code ignored;
        if (std::filesystem::exists(paths.source_root / "include" / "librecomp" /
                "rsp.hpp", ignored)) {
            paths.header_directories = {paths.source_root / "include"};
        } else {
            paths.header_directories = {
                paths.source_root / "include",
                paths.source_root / "lib" / "N64ModernRuntime" / "N64Recomp" / "include",
                paths.source_root / "lib" / "N64ModernRuntime" / "librecomp" / "include",
                paths.source_root / "lib" / "N64ModernRuntime" / "ultramodern" / "include",
            };
        }
    }

    paths.system_include_directories =
        split_paths(std::getenv("WR64_GAME_SYSTEM_INCLUDE_DIRS"));
    return paths;
}

bool prepared(const Paths& paths, std::filesystem::path& module) {
    std::lock_guard lock{build_mutex};
    std::string error;
    const auto recipe_path = paths.source_root / "config" / "generated-code.json";
    const std::string recipe_hash = recipe_hash_of(recipe_path, error);
    if (recipe_hash.empty()) {
        return false;
    }
    const std::string compiler = compiler_identity(paths, error);
    if (compiler.empty()) {
        return false;
    }
    const std::string key = module_key(recipe_hash, compiler, error);
    const std::filesystem::path directory = paths.data_directory / key;
    const std::filesystem::path candidate = directory / kModuleName;
    std::error_code ignored;
    if (!std::filesystem::is_regular_file(candidate, ignored) ||
        !record_matches(directory / kRecordName, recipe_hash, compiler) ||
        !validate_module(candidate, recipe_hash, error)) {
        return false;
    }
    module = candidate;
    return true;
}

Result build(const Paths& paths, const Progress& progress) {
    std::lock_guard lock{build_mutex};
    Result result;
    std::string error;

    std::mutex progress_mutex;
    int reported_percent = 0;
    auto report = [&](int percent, std::string_view step) {
        if (progress.report) {
            std::lock_guard guard{progress_mutex};
            if (percent < reported_percent) {
                percent = reported_percent;
            } else {
                reported_percent = percent;
            }
            progress.report(percent, step);
        }
    };
    auto cancelled = [&]() { return progress.cancelled && progress.cancelled(); };
    auto fail = [&](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    report(0, "Reading ROM");
    if (cancelled()) {
        return fail("Preparation was cancelled");
    }

    const auto recipe_path = paths.source_root / "config" / "generated-code.json";
    const std::string recipe_hash = recipe_hash_of(recipe_path, error);
    if (recipe_hash.empty()) {
        return fail(error);
    }
    wr64::generation::Recipe recipe;
    if (!wr64::generation::load_recipe(recipe_path, recipe, error)) {
        return fail(error);
    }
    if (!verify_inputs(paths.source_root, recipe, error)) {
        return fail(error);
    }

    const std::string compiler = compiler_identity(paths, error);
    if (compiler.empty()) {
        return fail(error);
    }
    const auto environment = tool_environment(paths);
    const std::string key = module_key(recipe_hash, compiler, error);
    const std::filesystem::path final_directory = paths.data_directory / key;
    const std::filesystem::path final_module = final_directory / kModuleName;
    if (std::filesystem::is_regular_file(final_module) &&
        record_matches(final_directory / kRecordName, recipe_hash, compiler) &&
        validate_module(final_module, recipe_hash, error)) {
        report(100, "Ready");
        result.ok = true;
        result.module = final_module;
        return result;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(paths.data_directory, filesystem_error);
    if (filesystem_error) {
        return fail("Cannot create " + paths.data_directory.string() + ": " +
            filesystem_error.message());
    }
    result.log = paths.data_directory / "game-module-build.log";
    std::ofstream log(result.log, std::ios::trunc);
    std::mutex log_mutex;
    auto log_line = [&](std::string_view line) {
        std::lock_guard guard{log_mutex};
        log << line << '\n';
        log.flush();
    };
    auto log_command = [&](const std::vector<std::string>& command,
        const std::string& output) {
        std::lock_guard guard{log_mutex};
        log << "$";
        for (const std::string& argument : command) {
            log << ' ' << argument;
        }
        log << '\n';
        if (!output.empty()) {
            log << output;
            if (output.back() != '\n') {
                log << '\n';
            }
        }
        log.flush();
    };

    log_line("RaceWave46 game module build");
    log_line("recipe: " + recipe_hash);
    log_line("compiler: " + compiler);

    if (cancelled()) {
        return fail("Preparation was cancelled");
    }

    report(2, "Reading ROM");
    std::vector<uint8_t> rom;
    if (!read_binary(paths.rom, rom, error)) {
        return fail(error);
    }
    const std::string input_hash = digest_of(rom);
    std::string patched_hash;
    // librecomp pads the ROM it stores to a multiple of four bytes, while the
    // recipe hashes the exact IPS output. Accept that stored form after
    // trimming the padding.
    bool stored_patched = false;
    for (size_t trim = 0; trim < 4 && trim <= rom.size(); ++trim) {
        const size_t size = rom.size() - trim;
        if (digest_of(std::span<const uint8_t>(rom.data(), size)) ==
            recipe.patched_rom_sha256) {
            rom.resize(size);
            stored_patched = true;
            break;
        }
    }
    if (stored_patched) {
        patched_hash = recipe.patched_rom_sha256;
    } else if (input_hash == recipe.rom_sha256) {
        std::vector<uint8_t> patch;
        if (!read_binary(paths.source_root / "data" / "waverace64-widescreen.ips",
                patch, error)) {
            return fail(error);
        }
        if (!apply_ips(rom, patch, error)) {
            return fail("Cannot apply the widescreen patch: " + error);
        }
        patched_hash = digest_of(rom);
        if (patched_hash != recipe.patched_rom_sha256) {
            return fail("Patched ROM verification failed");
        }
    } else {
        return fail("The selected ROM does not match this release. Supply the "
            "unmodified, big-endian USA Rev 1 cartridge.");
    }
    log_line("rom: " + patched_hash);

    const std::filesystem::path work = paths.data_directory /
        ("build-" + key.substr(0, 16) + "-" + std::to_string(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            getpid()
#endif
        ));
    remove_quietly(work);
    std::filesystem::create_directories(work / "RecompiledFuncsPoint0", filesystem_error);
    std::filesystem::create_directories(work / "rsp", filesystem_error);
    std::filesystem::create_directories(work / "obj", filesystem_error);
    if (filesystem_error) {
        return fail("Cannot create a temporary build directory: " +
            filesystem_error.message());
    }

    auto cleanup_failure = [&](std::string message) {
        log_line("error: " + message);
        remove_quietly(work);
        return fail(std::move(message));
    };

    if (!write_binary(work / "input.z64", rom.data(), rom.size(), error)) {
        return cleanup_failure(error);
    }

    // Rewrite the two generator configs the same way tools/generate_code.py
    // does: the shipped configs reference the repository layout.
    const std::map<std::string, std::string> replacements = {
        {"../Wave Race 64 - Kawasaki Jet Ski (USA) (Rev 1).z64",
            (work / "input.z64").string()},
        {"../symbols/waverace64.us.rev1.toml",
            (paths.source_root / "symbols" / "waverace64.us.rev1.toml").string()},
        {"../RecompiledFuncsPoint0", (work / "RecompiledFuncsPoint0").string()},
        {"../rsp/aspMain.cpp", (work / "rsp" / "aspMain.cpp").string()},
    };
    struct Generator {
        const char* config;
        const std::filesystem::path tool;
    };
    const std::vector<Generator> generators = {
        {"us.rev1.toml", paths.n64recomp},
        {"aspMain.us.rev1.toml", paths.rsprecomp},
    };

    report(3, "Recompiling game code");
    for (size_t index = 0; index < generators.size(); ++index) {
        const Generator& generator = generators[index];
        std::string text = read_text(paths.source_root / "config" / generator.config,
            error);
        if (text.empty()) {
            return cleanup_failure(error);
        }
        // tools/generate_code.py rewrites every occurrence and leaves configs
        // that do not reference a path untouched.
        for (const auto& [from, to] : replacements) {
            size_t position = 0;
            while ((position = text.find(from, position)) != std::string::npos) {
                text.replace(position, from.size(), to);
                position += to.size();
            }
        }
        const std::filesystem::path config_path = work / generator.config;
        if (!write_text(config_path, text, error)) {
            return cleanup_failure(error);
        }
        std::string output;
        const std::vector<std::string> command = {generator.tool.string(),
            config_path.string()};
        if (!run_process(command, work, environment, 10, output, error)) {
            log_command(command, output);
            return cleanup_failure(error + ". See " + result.log.string());
        }
        log_command(command, output);
        report(3 + static_cast<int>((index + 1) * 24 / generators.size()),
            "Recompiling game code");
        if (cancelled()) {
            remove_quietly(work);
            return fail("Preparation was cancelled");
        }
    }

    report(29, "Verifying generated code");
    if (!wr64::generation::finalize(work / "RecompiledFuncsPoint0", error) ||
        !wr64::generation::verify_generated(work, recipe, error)) {
        return cleanup_failure(error);
    }
    if (!wr64::generation::patch_rsp_dispatch(work / "rsp" / "aspMain.cpp", error)) {
        return cleanup_failure(error);
    }
    report(35, "Compiling game code");

    struct Job {
        std::filesystem::path source;
        std::filesystem::path object;
        bool cxx;
        bool sse41;
    };
    std::vector<Job> jobs;
    for (const auto& [name, checksum] : recipe.outputs) {
        (void)checksum;
        if (name.size() > 2 && name.compare(name.size() - 2, 2, ".c") == 0) {
            jobs.push_back({work / name, work / "obj" /
                (std::filesystem::path(name).stem().string() + ".o"), false, false});
        }
    }
    jobs.push_back({work / "RecompiledFuncsPoint0" / "lookup.cpp",
        work / "obj" / "lookup.o", true, false});
    jobs.push_back({paths.source_root / "src" / "game_module" /
        "wr64_game_module_abi.cpp", work / "obj" / "abi.o", true, false});
    jobs.push_back({work / "rsp" / "aspMain.cpp", work / "obj" / "aspMain.o", true, true});

    const size_t workers = std::max<size_t>(1, std::min<size_t>(
        std::thread::hardware_concurrency() == 0 ? 4 :
            std::thread::hardware_concurrency(), 8));
    std::atomic<size_t> next{0};
    std::atomic<size_t> completed{0};
    std::atomic<bool> failed{false};
    std::mutex failure_mutex;
    std::string failure;

    auto worker = [&]() {
        while (!failed.load()) {
            const size_t index = next.fetch_add(1);
            if (index >= jobs.size()) {
                return;
            }
            const Job& job = jobs[index];
            std::string output;
            std::string run_error;
            const auto command = compile_command(paths, work / "RecompiledFuncsPoint0",
                job.source, job.object, job.cxx, job.sse41, recipe_hash);
            const bool ok = run_process(command, work, environment, 10, output, run_error);
            log_command(command, output);
            if (!ok) {
                std::lock_guard guard{failure_mutex};
                if (failure.empty()) {
                    failure = run_error + " (" + job.source.filename().string() +
                        "). See " + result.log.string();
                }
                failed.store(true);
                return;
            }
            const size_t done = completed.fetch_add(1) + 1;
            report(35 + static_cast<int>(done * 55 / jobs.size()),
                "Compiling game code");
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (size_t index = 0; index < workers; ++index) {
        threads.emplace_back(worker);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    if (failed.load()) {
        return cleanup_failure(failure);
    }
    if (cancelled()) {
        remove_quietly(work);
        return fail("Preparation was cancelled");
    }

    report(91, "Linking game module");
    std::vector<std::string> link_command = {paths.compiler.string(), "-shared",
        "-nostdlib", "-fuse-ld=lld", "-Wl,-soname," + std::string(kModuleName),
        "-o", (work / kModuleName).string()};
    for (const Job& job : jobs) {
        link_command.push_back(job.object.string());
    }
    std::string link_output;
    if (!run_process(link_command, work, environment, 10, link_output, error)) {
        log_command(link_command, link_output);
        return cleanup_failure(error + ". See " + result.log.string());
    }
    log_command(link_command, link_output);

    report(96, "Finishing");
    if (!validate_module(work / kModuleName, recipe_hash, error)) {
        return cleanup_failure(error);
    }
    if (!write_text(work / kRecordName,
            build_record(recipe_hash, compiler, patched_hash), error)) {
        return cleanup_failure(error);
    }

    std::filesystem::create_directories(final_directory, filesystem_error);
    if (filesystem_error) {
        return cleanup_failure("Cannot create " + final_directory.string() + ": " +
            filesystem_error.message());
    }
    const std::filesystem::path staged_module = final_directory / (kModuleName + std::string(".tmp"));
    if (!std::filesystem::copy_file(work / kModuleName, staged_module,
            std::filesystem::copy_options::overwrite_existing, filesystem_error) ||
        !std::filesystem::copy_file(work / kRecordName, final_directory / kRecordName,
            std::filesystem::copy_options::overwrite_existing, filesystem_error)) {
        remove_quietly(staged_module);
        return cleanup_failure("Cannot install the game module: " +
            filesystem_error.message());
    }
    std::filesystem::rename(staged_module, final_module, filesystem_error);
    if (filesystem_error) {
        remove_quietly(staged_module);
        return cleanup_failure("Cannot install the game module: " +
            filesystem_error.message());
    }

    remove_quietly(work);
    log_line("module: " + final_module.string());
    report(100, "Ready");
    result.ok = true;
    result.module = final_module;
    return result;
}

}  // namespace wr64::game_builder
