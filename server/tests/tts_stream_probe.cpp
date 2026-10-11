// Opt-in physical audio probe. Not registered with CTest and not shipped.
#include "english/pcm_stream.h"
#include "english/pcm_player.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4)
        return 2;
    std::wstring modelW = argv[2], voiceW = argv[3];
    std::string model(modelW.begin(), modelW.end()), voice(voiceW.begin(), voiceW.end()), token;
    std::ifstream(std::filesystem::path(argv[1]) / "service-token.txt") >> token;
    if (token.size() != 64)
        return 3;
    std::atomic<uint64_t> generation{1};
    auto start = std::chrono::steady_clock::now();
    double firstMs = -1;
    unsigned chunks = 0;
    std::thread cancel;
    Speech::PcmPlayer player(generation, 1, 35);
    Speech::PcmStream parser([&](unsigned rate, unsigned channels, std::string pcm) {
        player.Write(rate, channels, std::move(pcm));
        if (!chunks++)
        {
            firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "first_pcm_queued_ms=" << firstMs << std::endl;
            if (argc > 4)
                cancel = std::thread([&] {
                    Sleep(100);
                    ++generation;
                });
        }
    });
    struct Context
    {
        Speech::PcmStream *parser;
        std::string error;
    } context{&parser, {}};
    CURL *curl = curl_easy_init();
    curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    std::string body =
        nlohmann::json{{"model", model},
                       {"voice", voice},
                       {"language", "zh"},
                       {"speed", 1},
                       {"response_format", "wav"},
                       {"stream", true},
                       {"input", argc > 4
                                     ? "再次测试停止播放。今天测试流式发音，第一段声音出来后，后续的声音会继续生成。"
                                     : "今天测试流式发音，第一段声音出来后，后续的声音会继续生成。"}}
            .dump();
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:18767/v1/audio/speech");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &generation);
    curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION, +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<std::atomic<uint64_t> *>(p)->load() != 1;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](char *p, size_t size, size_t count, void *ctx) -> size_t {
            auto &c = *static_cast<Context *>(ctx);
            try
            {
                c.parser->Feed(std::string_view(p, size * count));
                return size * count;
            }
            catch (const std::exception &e)
            {
                c.error = e.what();
                return 0;
            }
        });
    const auto result = curl_easy_perform(curl);
    const auto receivedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (cancel.joinable())
        cancel.join();
    try
    {
        if (argc > 4)
        {
            if (generation != 2 || chunks == 0 || result == CURLE_OK)
                throw std::runtime_error("cancellation did not interrupt transport");
        }
        else
        {
            if (result != CURLE_OK || status != 200)
                throw std::runtime_error(context.error.empty() ? "HTTP failed" : context.error);
            parser.Finish();
            player.Finish();
            if (chunks < 2 || firstMs >= receivedMs)
                throw std::runtime_error("not an incremental stream");
        }
        std::cout << nlohmann::json{{"model", model},
                                    {"first_queued_ms", firstMs},
                                    {"transport_ms", receivedMs},
                                    {"chunks", chunks},
                                    {"cancelled", argc > 4}}
                         .dump()
                  << std::endl;
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
