#include "spirv_glsl.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: probe shader.spv [required_GL_extension]\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "cannot open SPIR-V\n";
        return 1;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint32_t> spirv((static_cast<size_t>(size) + 3) / 4);
    if (!file.read(reinterpret_cast<char *>(spirv.data()), size)) {
        std::cerr << "cannot read SPIR-V\n";
        return 1;
    }
    try {
        spirv_cross::CompilerGLSL compiler(spirv);
        spirv_cross::CompilerGLSL::Options options;
        options.version = 120;
        options.es = false;
        options.enable_420pack_extension = false;
        options.emit_push_constant_as_uniform_buffer = false;
        compiler.set_common_options(options);
        if (argc == 3) compiler.require_extension(argv[2]);
        std::string glsl = compiler.compile();
        std::cout << glsl;
        const auto &extensions = compiler.get_required_extensions();
        std::cerr << "REQUIRED_EXTENSIONS_COUNT " << extensions.size() << '\n';
        for (const auto &extension : extensions)
            std::cerr << "REQUIRED_EXTENSION " << extension << '\n';
    } catch (const std::exception &error) {
        std::cerr << "SPIRV-Cross error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
