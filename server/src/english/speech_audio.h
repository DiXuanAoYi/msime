#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Speech
{
struct PcmAudio
{
    unsigned rate = 0, channels = 0;
    std::string pcm;
};
inline std::string EncodeWav(const PcmAudio &audio)
{
    if (audio.rate < 8000 || audio.rate > 48000 || audio.channels < 1 || audio.channels > 2 || audio.pcm.empty() ||
        audio.pcm.size() % (audio.channels * 2) || audio.pcm.size() > size_t(audio.rate) * audio.channels * 2 * 90)
        throw std::runtime_error("音频格式无效或超过 90 秒");
    std::string out = "RIFF";
    auto u32 = [&](uint32_t n) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<char>(n >> (8 * i)));
    };
    auto u16 = [&](uint16_t n) {
        out.push_back(static_cast<char>(n));
        out.push_back(static_cast<char>(n >> 8));
    };
    u32(static_cast<uint32_t>(audio.pcm.size() + 36));
    out += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(static_cast<uint16_t>(audio.channels));
    u32(audio.rate);
    u32(audio.rate * audio.channels * 2);
    u16(static_cast<uint16_t>(audio.channels * 2));
    u16(16);
    out += "data";
    u32(static_cast<uint32_t>(audio.pcm.size()));
    out += audio.pcm;
    return out;
}
inline PcmAudio ParseWav(std::string_view input)
{
    auto u32 = [&](size_t p) {
        uint32_t n;
        memcpy(&n, input.data() + p, 4);
        return n;
    };
    auto u16 = [&](size_t p) {
        uint16_t n;
        memcpy(&n, input.data() + p, 2);
        return n;
    };
    if (input.size() < 44 || input.substr(0, 4) != "RIFF" || input.substr(8, 4) != "WAVE" || u32(4) != input.size() - 8)
        throw std::runtime_error("WAV 文件不完整");
    PcmAudio result;
    bool format = false;
    for (size_t pos = 12; pos + 8 <= input.size();)
    {
        size_t size = u32(pos + 4);
        if (size > input.size() - pos - 8)
            throw std::runtime_error("WAV 数据块越界");
        if (input.substr(pos, 4) == "fmt ")
        {
            if (size < 16 || u16(pos + 8) != 1 || u16(pos + 22) != 16)
                throw std::runtime_error("需要 PCM16 WAV 音频");
            result.channels = u16(pos + 10);
            result.rate = u32(pos + 12);
            format = true;
        }
        if (input.substr(pos, 4) == "data")
            result.pcm.append(input.substr(pos + 8, size));
        pos += 8 + size + (size & 1);
    }
    if (!format)
        throw std::runtime_error("WAV 缺少格式信息");
    EncodeWav(result); // Apply the same duration, channel and alignment limits.
    return result;
}
} // namespace Speech
