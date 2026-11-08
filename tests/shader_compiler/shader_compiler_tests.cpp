#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shader_compiler;
import ngine.render;

// Slang zur Laufzeit (R2.4, ADR 0034): übersetzen ohne GPU, dann Hot Reload der Renderer-Shader.

using namespace ngine;

namespace {

bool gpu_required() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv("NGINE_REQUIRE_GPU");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && *value != '\0';
}

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_slang_" + std::to_string(random()));
        const std::u8string text = base.u8string();
        path_.assign(text.begin(), text.end());
    }
    ~TempDir() {
        static_cast<void>(filesystem::remove(path_));
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(std::string_view name) const {
        return filesystem::join(path_, name);
    }

private:
    std::string path_;
};

shader_compiler::Compiler& compiler() {
    static std::unique_ptr<shader_compiler::Compiler> instance = [] {
        auto created = shader_compiler::Compiler::create();
        REQUIRE(created);
        return *std::move(created);
    }();
    return *instance;
}

constexpr std::string_view farbe_slang = R"(
import farbe_teil;

[shader("fragment")]
float4 fragment_main() : SV_Target
{
    return float4(teil_farbe(), 1.0);
}
)";

constexpr std::string_view teil_slang = R"(
module farbe_teil;

public float3 teil_farbe() { return float3(1.0, 0.5, 0.25); }
)";

} // namespace

TEST_CASE("Slang compiles a shader with imports to SPIR-V at runtime", "[shader]") {
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("farbe.slang"), farbe_slang));
    REQUIRE(filesystem::write_text(dir.file("farbe_teil.slang"), teil_slang));

    const auto spirv =
        compiler().compile(dir.file("farbe.slang"), "fragment_main", rhi::ShaderStage::Fragment);
    REQUIRE(spirv);
    REQUIRE(spirv->size() > 5);
    CHECK((*spirv)[0] == 0x07230203u); // SPIR-V-Kennung

    // Jeder Aufruf liest die Dateien neu, auch importierte.
    REQUIRE(filesystem::write_text(dir.file("farbe_teil.slang"),
                                   "module farbe_teil;\npublic float3 teil_farbe() { return "
                                   "float3(0.0); }\n"));
    CHECK(compiler().compile(dir.file("farbe.slang"), "fragment_main", rhi::ShaderStage::Fragment));
}

TEST_CASE("Slang reports errors with file and line", "[shader]") {
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("kaputt.slang"),
                                   "[shader(\"fragment\")]\nfloat4 fragment_main() : SV_Target\n"
                                   "{\n    return undefiniert;\n}\n"));
    const auto broken =
        compiler().compile(dir.file("kaputt.slang"), "fragment_main", rhi::ShaderStage::Fragment);
    REQUIRE_FALSE(broken);
    INFO(broken.error());
    CHECK(broken.error().find("kaputt.slang") != std::string::npos);
    CHECK(broken.error().find("undefiniert") != std::string::npos);

    CHECK_FALSE(
        compiler().compile(dir.file("fehlt.slang"), "fragment_main", rhi::ShaderStage::Fragment));
    REQUIRE(filesystem::write_text(dir.file("leer.slang"), "float3 nichts() { return 0; }\n"));
    CHECK_FALSE(
        compiler().compile(dir.file("leer.slang"), "fragment_main", rhi::ShaderStage::Fragment));
}

TEST_CASE("Renderer reloads its shaders from source and keeps them on errors",
          "[shader][render][gpu]") {
    auto created =
        rhi::vulkan::create_device({.application_name = "NGine Tests", .enable_validation = true});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
    }
    const std::unique_ptr<rhi::Device> device = *std::move(created);
    auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(renderer);

    const std::string shader_dir = NGINE_SHADER_SOURCE_DIR;
    const render::ShaderSource from_source = [&](std::string_view file, std::string_view entry,
                                                 rhi::ShaderStage stage) {
        return compiler().compile(filesystem::join(shader_dir, file), entry, stage);
    };
    const auto reloaded = (*renderer)->reload_shaders(from_source);
    INFO((reloaded ? std::string{} : reloaded.error()));
    CHECK(reloaded);

    // Ein Fehler lässt alles beim Alten.
    const render::ShaderSource failing = [](std::string_view, std::string_view,
                                            rhi::ShaderStage) -> rhi::Result<std::vector<u32>> {
        return std::unexpected(std::string{"absichtlich"});
    };
    const auto failed = (*renderer)->reload_shaders(failing);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().find("absichtlich") != std::string::npos);
    CHECK(device->validation_errors() == 0);
}
