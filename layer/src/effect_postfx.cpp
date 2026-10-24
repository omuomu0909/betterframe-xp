#include "effect_postfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "buffer.hpp"
#include "descriptor_set.hpp"
#include "shader_sources.hpp"
#include "stream_gate.hpp"

namespace vkBasalt
{
    namespace
    {
        std::string trim(const std::string& value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        std::string defaultParamsPath()
        {
            const char* env = std::getenv("BETTERFRAME_XP_PARAMS");
            if (env && *env) return env;
            const char* xdg  = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            if (xdg && *xdg) return std::string(xdg) + "/betterframe-xp/params.conf";
            if (home && *home) return std::string(home) + "/.config/betterframe-xp/params.conf";
            return {};
        }

        // The colour grade is a no-op when every value is 1.0; the layer then skips the pass entirely.
        bool colorNeutral(const PostFxParams& p)
        {
            return p.contrast == 1.0f && p.saturation == 1.0f && p.gamma == 1.0f
                && p.temperature == 1.0f && p.brightness == 1.0f;
        }

        void resetColor(PostFxParams& p)
        {
            p.contrast    = 1.0f;
            p.saturation  = 1.0f;
            p.gamma       = 1.0f;
            p.temperature = 1.0f;
            p.brightness  = 1.0f;
        }
    } // namespace

    bool readPostFxParams(const std::string& path, PostFxParams& params, bool* streamOnly)
    {
        std::ifstream input(path);
        if (!input) return false;
        PostFxParams next = params;
        // "enabled = 0" turns every effect off without removing the layer.
        float enabled = 1.0f;
        float stream  = 0.0f;
        struct Field { const char* name; float* value; float lo; float hi; };
        const Field fields[] = {
            {"enabled",         &enabled,              0.0f, 1.0f},
            {"streamOnly",      &stream,               0.0f, 1.0f},
            {"sharpness",       &next.sharpness,       0.0f, 1.0f},
            {"sharpenAmount",   &next.sharpenAmount,   0.0f, 2.0f},
            {"debandAmount",    &next.debandAmount,    0.0f, 1.0f},
            {"debandRange",     &next.debandRange,     1.0f, 32.0f},
            {"debandThreshold", &next.debandThreshold, 0.5f, 8.0f},
            {"centerRadius",    &next.centerRadius,    0.0f, 1.0f},
            {"edgeAmount",      &next.edgeAmount,      0.0f, 1.0f},
            {"splitEyes",       &next.splitEyes,       0.0f, 1.0f},
            {"showSplit",       &next.showSplit,       0.0f, 1.0f},
            {"detailBoost",     &next.detailBoost,     0.0f, 1.0f},
            {"grainAmount",     &next.grainAmount,     0.0f, 1.0f},
            {"grainSize",       &next.grainSize,       1.0f, 4.0f},
            {"grainAnimate",    &next.grainAnimate,    0.0f, 1.0f},
            {"casCross",        &next.casCross,        0.0f, 1.0f},
            {"casLuma",         &next.casLuma,         0.0f, 1.0f},
            {"cornerSkip",      &next.cornerSkip,      0.0f, 2.0f},
            {"contrast",        &next.contrast,        0.5f, 1.5f},
            {"saturation",      &next.saturation,      0.0f, 2.0f},
            {"gamma",           &next.gamma,           0.5f, 2.5f},
            {"temperature",     &next.temperature,     0.8f, 1.2f},
            {"brightness",      &next.brightness,      0.5f, 1.5f},
        };
        std::string line;
        while (std::getline(input, line))
        {
            const auto comment = line.find('#');
            if (comment != std::string::npos) line.erase(comment);
            const auto equal = line.find('=');
            if (equal == std::string::npos) continue;
            const std::string key = trim(line.substr(0, equal));
            const std::string val = trim(line.substr(equal + 1));
            char* end = nullptr;
            const float number = std::strtof(val.c_str(), &end);
            if (val.empty() || end == val.c_str() || !std::isfinite(number)) continue;
            for (const auto& field : fields)
                if (key == field.name) *field.value = std::clamp(number, field.lo, field.hi);
        }
        if (enabled < 0.5f)
        {
            next.sharpenAmount = 0.0f;
            next.debandAmount  = 0.0f;
            next.grainAmount   = 0.0f;
            resetColor(next);
        }
        params = next;
        if (streamOnly) *streamOnly = stream >= 0.5f;
        return true;
    }

    bool PostFxEffect::loadParams(PostFxParams& out)
    {
        PostFxParams next   = out;
        bool         stream = false;
        if (!readPostFxParams(paramsPath, next, &stream)) return false;
        streamOnly = stream;
        out        = next;
        return true;
    }

    PostFxEffect::PostFxEffect(LogicalDevice*       pLogicalDevice,
                               VkFormat             format,
                               VkExtent2D           imageExtent,
                               std::vector<VkImage> inputImages,
                               std::vector<VkImage> outputImages,
                               Config*              pConfig)
    {
        vertexCode        = full_screen_triangle_vert;
        fragmentCode      = postfx_frag;
        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = nullptr;

        current.texel[0] = 1.0f / static_cast<float>(imageExtent.width);
        current.texel[1] = 1.0f / static_cast<float>(imageExtent.height);
        paramsPath       = defaultParamsPath();
        if (!paramsPath.empty()) loadParams(current);

        createBuffer(pLogicalDevice,
                     sizeof(PostFxParams),
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     paramBuffer,
                     paramMemory);
        VkResult result = pLogicalDevice->vkd.MapMemory(pLogicalDevice->device, paramMemory, 0, sizeof(PostFxParams), 0, &paramMapped);
        ASSERT_VULKAN(result);
        written = current;
        std::memcpy(paramMapped, &written, sizeof(PostFxParams));

        paramSetLayout = createUniformBufferDescriptorSetLayout(pLogicalDevice);
        descriptorSetLayouts.push_back(paramSetLayout);

        VkDescriptorPoolSize poolSize;
        poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = 1;
        paramPool = createDescriptorPool(pLogicalDevice, {poolSize});

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig);

        paramSet = writeBufferDescriptorSet(pLogicalDevice, paramPool, paramSetLayout, paramBuffer);
        Logger::info("betterframe-xp effect ready, params: " + paramsPath);
    }

    PostFxEffect::~PostFxEffect()
    {
        pLogicalDevice->vkd.UnmapMemory(pLogicalDevice->device, paramMemory);
        pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, paramBuffer, nullptr);
        pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, paramMemory, nullptr);
        pLogicalDevice->vkd.DestroyDescriptorSetLayout(pLogicalDevice->device, paramSetLayout, nullptr);
        pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, paramPool, nullptr);
    }

    void PostFxEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        pLogicalDevice->vkd.CmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &paramSet, 0, nullptr);
        SimpleEffect::applyEffect(imageIndex, commandBuffer);
    }

    void PostFxEffect::updateEffect()
    {
        // Poll the file at most every 250 ms. Parameters live in a coherent
        // buffer, so the recorded command buffers stay valid.
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextCheck && !paramsPath.empty())
        {
            nextCheck = now + std::chrono::milliseconds(250);
            PostFxParams next = current;
            if (loadParams(next) && std::memcmp(&next, &current, sizeof(PostFxParams)) != 0)
            {
                current = next;
                Logger::info("betterframe-xp params updated");
            }
        }

        // What goes into the buffer this frame: the file's values, new grain seed (cheap: one float), and with
        // streamOnly the effects off unless the compositor drew the PC video lately. The draws are recorded
        // before this present, and the effect pass runs after them, so the flag is current for this frame.
        // A frame still on the GPU may see the next values; that costs at most one frame of difference.
        frameCount = (frameCount + 1) % 64;
        PostFxParams next = current;
        next.frameSeed    = static_cast<float>(frameCount);
        if (streamOnly)
        {
            const bool video = streamGate::videoRecent();
            if (video != videoShown)
            {
                videoShown = video;
                Logger::info(std::string("betterframe-xp PC video ") + (video ? "on screen: effects on" : "gone: effects off")
                             + " (video draws so far " + std::to_string(streamGate::videoDrawCount()) + ")");
            }
            if (!video)
            {
                next.sharpenAmount = 0.0f;
                next.debandAmount  = 0.0f;
                next.grainAmount   = 0.0f;
                resetColor(next);
            }
        }
        idle = next.sharpenAmount <= 0.0f && next.debandAmount <= 0.0f && next.grainAmount <= 0.0f
            && colorNeutral(next);
        if (std::memcmp(&next, &written, sizeof(PostFxParams)) != 0)
        {
            written = next;
            std::memcpy(paramMapped, &written, sizeof(PostFxParams));
        }
    }
} // namespace vkBasalt
