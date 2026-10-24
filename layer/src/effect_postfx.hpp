#ifndef EFFECT_POSTFX_HPP_INCLUDED
#define EFFECT_POSTFX_HPP_INCLUDED
#include <chrono>
#include <string>
#include <vector>

#include "vulkan_include.hpp"

#include "effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    // Matches the std140 "Params" block in shader/postfx.frag.glsl.
    struct PostFxParams
    {
        float texel[2];
        float sharpness       = 0.5f;
        float sharpenAmount   = 0.0f;
        float debandAmount    = 0.0f;
        float debandRange     = 12.0f;
        float debandThreshold = 2.5f;
        float centerRadius    = 0.45f;
        float edgeAmount      = 0.3f;
        float splitEyes       = 1.0f;
        float showSplit       = 0.0f;
        float detailBoost     = 0.0f;
        float grainAmount     = 0.0f;
        float grainSize       = 1.0f;
        float grainAnimate    = 1.0f;
        float casCross        = 1.0f;  // 1 = CAS cross taps only, 0 = full 3x3
        float casLuma         = 1.0f;  // 1 = CAS on luma, 0 = per colour channel
        float cornerSkip      = 1.15f; // corners beyond this eye radius are left alone, 0 = off
        float contrast        = 1.0f;  // 1 = unchanged (pivot 0.5)
        float saturation      = 1.0f;  // 1 = unchanged, 0 = greyscale
        float gamma           = 1.0f;  // 1 = unchanged, > 1 brightens mid-tones
        float temperature     = 1.0f;  // 1 = unchanged, > 1 warmer (overall luma preserved)
        float brightness      = 1.0f;  // 1 = unchanged
        float frameSeed       = 0.0f; // set by the layer every frame, not from the file
    };

    // Reads key = value lines. Unknown keys and bad values are ignored.
    // streamOnly (optional out): the file's "streamOnly" key, 1 = effects only while the PC video is being drawn.
    bool readPostFxParams(const std::string& path, PostFxParams& params, bool* streamOnly = nullptr);

    class PostFxEffect : public SimpleEffect
    {
    public:
        PostFxEffect(LogicalDevice*       pLogicalDevice,
                     VkFormat             format,
                     VkExtent2D           imageExtent,
                     std::vector<VkImage> inputImages,
                     std::vector<VkImage> outputImages,
                     Config*              pConfig);
        ~PostFxEffect();
        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void updateEffect() override;
        // True when the values updateEffect() just wrote leave the picture unchanged (sharpen, deband and grain
        // all zero and the colour grade neutral): the pass would only copy, so the layer may submit a plain image
        // copy instead.
        bool isIdle() const override { return idle; }

    private:
        bool                  idle              = false;
        VkBuffer              paramBuffer       = VK_NULL_HANDLE;
        VkDeviceMemory        paramMemory       = VK_NULL_HANDLE;
        void*                 paramMapped       = nullptr;
        VkDescriptorSetLayout paramSetLayout    = VK_NULL_HANDLE;
        VkDescriptorPool      paramPool         = VK_NULL_HANDLE;
        VkDescriptorSet       paramSet          = VK_NULL_HANDLE;
        PostFxParams          current{};         // what the file asks for
        PostFxParams          written{};         // what the buffer holds: current, minus the streamOnly gate, plus the seed
        std::string           paramsPath;
        uint32_t              frameCount        = 0;
        bool                  streamOnly        = false;
        bool                  videoShown        = false; // last state of the stream gate, for the log
        std::chrono::steady_clock::time_point nextCheck{};

        // Reads the file into out and sets streamOnly. Returns false if the file is unreadable.
        bool loadParams(PostFxParams& out);
    };
} // namespace vkBasalt

#endif // EFFECT_POSTFX_HPP_INCLUDED
