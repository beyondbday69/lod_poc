#include "mod/Config.h"

#include <filesystem>
#include <iostream>

namespace {

int usage(const char *program) {
    std::cerr << "usage: " << program << " <config-output-dir>\n";
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        return usage(argv[0]);
    }

    const std::filesystem::path configDir(argv[1]);

    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        std::cerr << "failed to create " << configDir << ": " << ec.message() << '\n';
        return 1;
    }

    pl::config::ConfigFile<lod_poc::ModConfig> config(
        lod_poc::ModConfig{}, configDir / "config.json",
        configDir / "config.schema.json");

    if (!config.save()) {
        std::cerr << "failed to write " << config.configPath() << '\n';
        return 1;
    }
    if (!config.writeSchema()) {
        std::cerr << "failed to write " << config.schemaPath() << '\n';
        return 1;
    }

    std::cout << "generated " << config.configPath() << '\n';
    std::cout << "generated " << config.schemaPath() << '\n';
    return 0;
}
