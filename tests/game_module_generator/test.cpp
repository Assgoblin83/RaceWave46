#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "wr64_game_builder.hpp"
#include "wr64_generation.hpp"
#include "wr64_sha256.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::printf("FAIL: %s\n", description);
        ++failures;
    }
}

bool read_text(const std::filesystem::path& path, std::string& text) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    return true;
}

void check_sha256() {
    check(wr64::crypto::to_hex(wr64::crypto::Sha256::hash(std::string_view{""})) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 empty vector");
    check(wr64::crypto::to_hex(wr64::crypto::Sha256::hash(std::string_view{"abc"})) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 abc vector");
}

void check_tokenizer() {
    check(wr64::generation::canonical_tokens("a  b\tc\n") == "a\nb\nc",
        "whitespace collapses");
    check(wr64::generation::canonical_tokens("// comment\nint x = 0x10;") ==
        "int\nx\n=\n0x10\n;",
        "line comments are dropped");
    check(wr64::generation::canonical_tokens("/* block\ncomment */x") == "x",
        "block comments are dropped");
    // The RSP dispatch cases are sorted so their order does not affect the
    // release hash.
    const std::string unsorted =
        "switch (x) {\n"
        "case 0x2: goto L_2;\n"
        "case 0x1: goto L_1;\n"
        "}\n";
    const std::string sorted =
        "switch (x) {\n"
        "case 0x1: goto L_1;\n"
        "case 0x2: goto L_2;\n"
        "}\n";
    check(wr64::generation::canonical_tokens(unsorted) ==
        wr64::generation::canonical_tokens(sorted),
        "dispatch cases are ordered");
}

void check_release_sources(const std::filesystem::path& root) {
    wr64::generation::Recipe recipe;
    std::string error;
    const bool loaded = wr64::generation::load_recipe(
        root / "config" / "generated-code.json", recipe, error);
    check(loaded, "recipe loads");
    if (!loaded) {
        std::printf("%s\n", error.c_str());
        return;
    }
    const bool verified = wr64::generation::verify_generated(root, recipe, error);
    check(verified, "committed generated code matches the recipe");
    if (!verified) {
        std::printf("%s\n", error.c_str());
    }
}

void check_rsp_patch(const std::filesystem::path& root) {
    const auto source = root / "rsp" / "aspMain.cpp";
    const auto temporary = std::filesystem::temp_directory_path() /
        "wr64-game-module-rsp-test.cpp";
    std::error_code ignored;
    std::filesystem::copy_file(source, temporary,
        std::filesystem::copy_options::overwrite_existing, ignored);
    std::string error;
    check(wr64::generation::patch_rsp_dispatch(temporary, error),
        "RSP dispatch patch applies");
    std::string patched;
    read_text(temporary, patched);
    check(patched.find("dispatch_targets[16]") != std::string::npos,
        "RSP dispatch table is inserted");
    check(!wr64::generation::patch_rsp_dispatch(temporary, error),
        "RSP dispatch patch refuses to run twice");
    std::filesystem::remove(temporary, ignored);
}

void check_builder(const std::filesystem::path& root,
    const std::filesystem::path& rom, const std::filesystem::path& data) {
    std::error_code ignored;
    std::filesystem::remove_all(data, ignored);

    wr64::game_builder::Paths paths =
        wr64::game_builder::default_paths(rom, data);
    check(paths.source_root == root, "source root resolves from the environment");

    wr64::game_builder::Progress progress;
    int last_percent = -1;
    bool ordered = true;
    progress.report = [&](int percent, std::string_view) {
        if (percent < last_percent) {
            ordered = false;
        }
        last_percent = percent;
    };
    const wr64::game_builder::Result result =
        wr64::game_builder::build(paths, progress);
    check(result.ok, "module builds from the cartridge image");
    if (!result.ok) {
        std::printf("%s\n", result.error.c_str());
        return;
    }
    check(ordered, "progress never goes backwards");
    check(last_percent == 100, "progress reaches 100%");
    check(std::filesystem::is_regular_file(result.module), "module is installed");

    std::filesystem::path prepared;
    check(wr64::game_builder::prepared(paths, prepared), "module is reused");
    check(prepared == result.module, "reuse finds the installed module");

#ifndef _WIN32
    unsetenv("WR64_BUILDER_NO_DLOPEN");
    {
        std::ofstream corrupt(result.module, std::ios::binary | std::ios::trunc);
        corrupt << "not a shared object";
    }
    check(!wr64::game_builder::prepared(paths, prepared),
        "corrupt cached module is rejected");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s ROOT [ROM DATA_DIRECTORY]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path root = argv[1];

    check_sha256();
    check_tokenizer();
    check_release_sources(root);
    check_rsp_patch(root);
    if (argc >= 4) {
        check_builder(root, argv[2], argv[3]);
    }

    if (failures == 0) {
        std::printf("game_module_generator: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
