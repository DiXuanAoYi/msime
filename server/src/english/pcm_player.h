#pragma once
#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <stdexcept>

namespace Speech
{
// waveOut consumes stable PCM buffers while the model is still generating.
class PcmPlayer
{
    struct Buffer
    {
        WAVEHDR header{};
        std::string pcm;
    };
    HWAVEOUT device_ = nullptr;
    std::deque<std::unique_ptr<Buffer>> buffers_;
    std::atomic<uint64_t> &generation_;
    uint64_t request_;
    int volume_;
    std::mutex deviceMutex_;
    std::atomic<bool> quit_{false};
    std::thread monitor_;
    void Check()
    {
        if (generation_.load() != request_)
            throw std::runtime_error("已停止朗读");
    }
    void Reap(bool wait)
    {
        while (!buffers_.empty())
        {
            Check();
            auto &buffer = buffers_.front();
            if (!(buffer->header.dwFlags & WHDR_DONE))
            {
                if (!wait)
                    return;
                Sleep(10);
                continue;
            }
            waveOutUnprepareHeader(device_, &buffer->header, sizeof(WAVEHDR));
            buffers_.pop_front();
            wait = false;
        }
    }

  public:
    PcmPlayer(std::atomic<uint64_t> &generation, uint64_t request, int volume)
        : generation_(generation), request_(request), volume_(volume), monitor_([this] {
              while (!quit_)
              {
                  if (generation_.load() != request_)
                  {
                      std::lock_guard lock(deviceMutex_);
                      if (device_)
                          waveOutReset(device_);
                      return;
                  }
                  Sleep(10);
              }
          })
    {
    }
    ~PcmPlayer()
    {
        quit_ = true;
        monitor_.join();
        if (device_)
        {
            waveOutReset(device_);
            for (auto &buffer : buffers_)
                waveOutUnprepareHeader(device_, &buffer->header, sizeof(WAVEHDR));
            waveOutClose(device_);
        }
    }
    void Write(unsigned rate, unsigned channels, std::string pcm)
    {
        Check();
        Reap(buffers_.size() >= 4);
        std::lock_guard lock(deviceMutex_);
        Check();
        if (!device_)
        {
            WAVEFORMATEX format{WAVE_FORMAT_PCM,
                                static_cast<WORD>(channels),
                                rate,
                                rate * channels * 2,
                                static_cast<WORD>(channels * 2),
                                16,
                                0};
            if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
                throw std::runtime_error("无法打开流式播放设备");
        }
        auto buffer = std::make_unique<Buffer>();
        buffer->pcm = std::move(pcm);
        for (size_t i = 0; i + 1 < buffer->pcm.size(); i += 2)
        {
            int16_t sample;
            memcpy(&sample, buffer->pcm.data() + i, 2);
            sample = static_cast<int16_t>(int(sample) * volume_ / 100);
            memcpy(buffer->pcm.data() + i, &sample, 2);
        }
        buffer->header.lpData = buffer->pcm.data();
        buffer->header.dwBufferLength = static_cast<DWORD>(buffer->pcm.size());
        if (waveOutPrepareHeader(device_, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            throw std::runtime_error("无法准备流式音频");
        buffers_.push_back(std::move(buffer));
        if (waveOutWrite(device_, &buffers_.back()->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            throw std::runtime_error("无法播放音频数据块");
    }
    void Finish()
    {
        while (!buffers_.empty())
            Reap(true);
    }
};
} // namespace Speech
