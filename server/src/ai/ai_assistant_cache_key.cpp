#include "ai_assistant_cache_key.h"

#include <nlohmann/json.hpp>

namespace AiAssistant::detail
{
std::string BuildSuggestionCacheKey(const Request &request)
{
    nlohmann::json localCandidates = nlohmann::json::array();
    if (request.config.provider == "local")
        for (const auto &answer : request.source_candidates)
            localCandidates.push_back({{"text", answer.text}, {"pinyin", answer.pinyin}});
    return nlohmann::json{{"provider", request.config.provider},
                          {"local_candidates", localCandidates},
                          {"task", TaskName(request.task)},
                          {"text", request.text},
                          {"thinking", request.config.thinking},
                          {"endpoint", request.config.endpoint},
                          {"model", request.config.model},
                          {"prompt", request.config.prompt},
                          {"pinyin_segments", request.pinyin_segments},
                          {"context", request.context},
                          {"candidate_limit", request.config.candidate_limit}}
        // context 与 prompt 是自由文本；键在无 try 的工作线程里构建，非法 UTF-8 不能抛出
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
} // namespace AiAssistant::detail
