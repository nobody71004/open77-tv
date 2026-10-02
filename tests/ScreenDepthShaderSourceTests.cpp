// The depth pass compiles its shaders at start-up from text embedded in
// ScreenDepthShaderSource.hpp. This fails while that text and the two shader files
// beside it (ScreenDepthComposite.hlsl, ScreenDepth.hlsli) disagree: an edit to a
// shader that was not carried into the header would otherwise ship unnoticed.

#include "webui/ScreenDepthShaderSource.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef OP77_WEBUI_SOURCE_DIR
#error "OP77_WEBUI_SOURCE_DIR must name client/src/webui"
#endif

namespace
{
/// A file's text with Windows line endings made Unix ones: a checkout may convert
/// them, and a raw string literal in the header holds plain newlines either way.
std::string ReadText(const std::string& aPath)
{
    std::ifstream file(aPath, std::ios::binary);
    if (!file)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    std::string out;
    out.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n')
        {
            continue;
        }
        out += text[index];
    }
    return out;
}

bool Same(const char* aName, const char* aEmbedded)
{
    const std::string path = std::string(OP77_WEBUI_SOURCE_DIR) + "/" + aName;
    const std::string onDisk = ReadText(path);
    if (onDisk.empty())
    {
        std::cerr << "Cannot read " << path << "\n";
        return false;
    }
    if (onDisk != aEmbedded)
    {
        std::cerr << aName << " differs from ScreenDepthShaderSource.hpp: regenerate the header from it.\n";
        return false;
    }
    return true;
}
} // namespace

int main()
{
    namespace Source = op77::WorldOverlay::ScreenDepth::ShaderSource;
    const bool composite = Same("ScreenDepthComposite.hlsl", Source::kComposite);
    const bool include = Same("ScreenDepth.hlsli", Source::kInclude);
    const bool name = std::strcmp(Source::kIncludeName, "ScreenDepth.hlsli") == 0;
    if (!composite || !include || !name)
    {
        return 1;
    }
    std::cout << "ScreenDepthShaderSource: the embedded shaders are the files beside them\n";
    return 0;
}
