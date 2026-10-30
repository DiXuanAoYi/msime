#pragma once
#include "engine/contracts/tts_stream.h"
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Speech
{
// Incremental framing parser, shared by the audio device and transport tests.
class PcmStream
{
    std::string pending_;
    unsigned rate_ = 0, channels_ = 0;
    uint64_t total_ = 0;
    bool done_ = false;
    std::function<void(unsigned, unsigned, std::string)> emit_;
    static uint32_t Read(const char *p)
    {
        uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i)
            n |= uint32_t(static_cast<unsigned char>(p[i])) << (i * 8);
        return n;
    }

  public:
    explicit PcmStream(std::function<void(unsigned, unsigned, std::string)> emit) : emit_(std::move(emit))
    {
    }
    void Feed(std::string_view data)
    {
        using namespace TtsStreamContract;
        if (data.size() > max_chunk_bytes * 2 || pending_.size() + data.size() > max_chunk_bytes * 3)
            throw std::runtime_error("语音流缓存超限");
        pending_.append(data);
        if (!rate_)
        {
            if (pending_.size() < 16)
                return;
            if (pending_.compare(0, 8, magic) != 0)
                throw std::runtime_error("本地服务未返回 PCM 流，请更新并重启本地模型");
            rate_ = Read(pending_.data() + 8);
            channels_ = Read(pending_.data() + 12);
            if (rate_ < min_rate || rate_ > max_rate || !channels_ || channels_ > max_channels)
                throw std::runtime_error("语音流格式无效");
            pending_.erase(0, 16);
        }
        while (pending_.size() >= 4 && !done_)
        {
            auto size = Read(pending_.data());
            if (size > max_chunk_bytes || size % (channels_ * 2))
                throw std::runtime_error("语音流数据块无效");
            if (pending_.size() < size + 4)
                return;
            pending_.erase(0, 4);
            if (!size)
            {
                done_ = true;
                break;
            }
            total_ += size;
            if (total_ > uint64_t(rate_) * channels_ * 2 * max_audio_seconds)
                throw std::runtime_error("语音流超过 90 秒");
            auto pcm = pending_.substr(0, size);
            pending_.erase(0, size);
            emit_(rate_, channels_, std::move(pcm));
        }
        if (done_ && !pending_.empty())
            throw std::runtime_error("语音流结束后出现额外数据");
    }
    void Finish() const
    {
        if (!done_ || !total_ || !pending_.empty())
            throw std::runtime_error("语音生成中断，音频不完整");
    }
};
} // namespace Speech
