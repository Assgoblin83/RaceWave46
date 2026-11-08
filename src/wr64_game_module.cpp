#include "wr64_game_module.hpp"

#include <cstdio>

#include "wr64_runtime.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace wr64::game_module {
namespace {

#ifdef _WIN32
using ModuleHandle = HMODULE;
constexpr const char* kModuleName = "wr64game.dll";
#else
using ModuleHandle = void*;
constexpr const char* kModuleName = "libwr64game.so";
#endif

ModuleHandle handle = nullptr;
const Wr64GameModule* module = nullptr;
std::string error;

void* symbol(ModuleHandle library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(library, name));
#else
    return dlsym(library, name);
#endif
}

void close_library(ModuleHandle library) {
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
}

std::string loader_error(const char* fallback) {
#ifdef _WIN32
    return fallback;
#else
    const char* detail = dlerror();
    return detail != nullptr ? detail : fallback;
#endif
}

}  // namespace

std::filesystem::path default_path() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return kModuleName;
    }
    return std::filesystem::path(path).parent_path() / kModuleName;
#else
    std::error_code ignored;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", ignored);
    if (ignored || executable.empty()) {
        return kModuleName;
    }
    return executable.parent_path() / kModuleName;
#endif
}

bool load(const std::filesystem::path& path) {
    if (loaded()) {
        return true;
    }
    error.clear();

    std::error_code file_error;
    if (!std::filesystem::is_regular_file(path, file_error)) {
        error = "Game module not found: " + path.string();
        return false;
    }

#ifdef _WIN32
    ModuleHandle candidate = LoadLibraryExW(
        std::filesystem::absolute(path).c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (candidate == nullptr) {
        error = "Cannot load game module: " + path.string();
        return false;
    }
#else
    ModuleHandle candidate = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (candidate == nullptr) {
        error = "Cannot load game module " + path.string() + ": " +
            loader_error("dlopen failed");
        return false;
    }
#endif

    auto entry = reinterpret_cast<Wr64GameModuleEntry>(
        symbol(candidate, WR64_GAME_MODULE_ABI_FUNCTION));
    if (entry == nullptr) {
        error = "Game module does not export " WR64_GAME_MODULE_ABI_FUNCTION
            ": " + path.string();
        close_library(candidate);
        return false;
    }

    const Wr64GameModule* candidate_module = entry();
    if (candidate_module == nullptr) {
        error = "Game module returned no ABI description: " + path.string();
        close_library(candidate);
        return false;
    }
    if (candidate_module->abi_version != WR64_GAME_MODULE_ABI_VERSION) {
        char message[160];
        std::snprintf(message, sizeof(message),
            "Game module ABI %u does not match this build (%u): %s",
            candidate_module->abi_version, WR64_GAME_MODULE_ABI_VERSION,
            path.string().c_str());
        error = message;
        close_library(candidate);
        return false;
    }
#ifdef WR64_ENTRYPOINT_ADDRESS
    if (candidate_module->entrypoint_address !=
        static_cast<gpr>(static_cast<int32_t>(WR64_ENTRYPOINT_ADDRESS))) {
        error = "Game module targets a different entrypoint address: " + path.string();
        close_library(candidate);
        return false;
    }
#endif
    if (candidate_module->struct_size < sizeof(Wr64GameModule) ||
        candidate_module->entrypoint == nullptr ||
        candidate_module->asp_main == nullptr ||
        candidate_module->code_sections == nullptr ||
        candidate_module->overlay_table == nullptr) {
        error = "Game module ABI description is incomplete: " + path.string();
        close_library(candidate);
        return false;
    }

    handle = candidate;
    module = candidate_module;
    wr64::register_overlays();
    return true;
}

bool loaded() {
    return module != nullptr;
}

const Wr64GameModule& abi() {
    return *module;
}

const std::string& last_error() {
    return error;
}

void unload() {
    if (handle != nullptr) {
        close_library(handle);
        handle = nullptr;
    }
    module = nullptr;
    error.clear();
}

}  // namespace wr64::game_module
