cmake_minimum_required(VERSION 3.20)
project(Wr64GameModuleGeneratorTests LANGUAGES CXX)
enable_testing()
get_filename_component(WR64_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(RUNTIME "${WR64_ROOT}/lib/N64ModernRuntime")

add_executable(wr64_game_module_generator_test test.cpp
    "${WR64_ROOT}/src/wr64_generation.cpp"
    "${WR64_ROOT}/src/wr64_sha256.cpp"
    "${WR64_ROOT}/src/wr64_game_builder.cpp")
target_compile_features(wr64_game_module_generator_test PRIVATE cxx_std_20)
target_include_directories(wr64_game_module_generator_test PRIVATE
    "${WR64_ROOT}/include"
    "${WR64_ROOT}/src"
    "${RUNTIME}/thirdparty"
    "${RUNTIME}/N64Recomp/include"
    "${RUNTIME}/librecomp/include"
    "${RUNTIME}/ultramodern/include")
if(UNIX)
    target_link_libraries(wr64_game_module_generator_test PRIVATE dl pthread)
endif()

add_test(NAME game_module_generated_code
    COMMAND wr64_game_module_generator_test "${WR64_ROOT}")

if(WR64_TEST_ROM)
    add_test(NAME game_module_builder
        COMMAND "${CMAKE_COMMAND}" -E env
            "WR64_BUILDER_NO_DLOPEN=1"
            "WR64_GAME_SOURCE_ROOT=${WR64_ROOT}"
            "WR64_GAME_TOOLS_ROOT=${WR64_ROOT}/build-tools"
            "$<TARGET_FILE:wr64_game_module_generator_test>"
            "${WR64_ROOT}" "${WR64_TEST_ROM}"
            "${CMAKE_CURRENT_BINARY_DIR}/module-data")
    set_tests_properties(game_module_builder PROPERTIES TIMEOUT 900)
endif()
