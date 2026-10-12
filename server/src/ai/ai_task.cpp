#include "ai_task.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <curl/curl.h>
#include <utf8.h>

namespace AiAssistant
{
bool IsLocalRerankResponse(Task task, const std::string &provider, const std::string &endpoint,
                           const nlohmann::json &response)
{
    const auto capability = response.find("capability");
    return task == Task::Pinyin && provider == "local" && IsLoopbackEndpoint(endpoint) && response.is_object() &&
           capability != response.end() && capability->is_string() && *capability == "rerank";
}
std::string TaskName(Task task)
{
    switch (task)
    {
    case Task::Completion:
        return "completion";
    case Task::Question:
        return "question";
    default:
        return "pinyin";
    }
}
Task ParseTask(const std::string &task)
{
    if (task == "completion")
        return Task::Completion;
    if (task == "question")
        return Task::Question;
    if (task == "pinyin")
        return Task::Pinyin;
    throw std::runtime_error("未知 AI 输入任务");
}
bool IsLoopbackEndpoint(const std::string &endpoint)
{
    CURLU *url = curl_url();
    if (!url)
        return false;
    char *host = nullptr, *scheme = nullptr, *user = nullptr;
    bool valid = curl_url_set(url, CURLUPART_URL, endpoint.c_str(), 0) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK;
    if (valid)
    {
        std::string h(host), s(scheme);
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        valid = (s == "http" || s == "https") && (h == "127.0.0.1" || h == "localhost" || h == "[::1]" || h == "::1");
        if (curl_url_get(url, CURLUPART_USER, &user, 0) == CURLUE_OK)
            valid = false;
    }
    curl_free(user);
    curl_free(host);
    curl_free(scheme);
    curl_url_cleanup(url);
    return valid;
}
std::vector<Answer> LookupOfflineQuestion(const std::string &text)
{
    struct Entry
    {
        const char *component;
        const char *pinyin;
        int count;
        const char *answer;
        const char *reading;
    };
    static constexpr Entry entries[] = {
        {"木", "mu", 2, "林", "lín"},    {"木", "mu", 3, "森", "sēn"},  {"人", "ren", 2, "从", "cóng"},
        {"人", "ren", 3, "众", "zhòng"}, {"火", "huo", 2, "炎", "yán"}, {"火", "huo", 3, "焱", "yàn"},
        {"水", "shui", 3, "淼", "miǎo"}, {"土", "tu", 2, "圭", "guī"},  {"土", "tu", 3, "垚", "yáo"},
        {"金", "jin", 3, "鑫", "xīn"},   {"日", "ri", 3, "晶", "jīng"}, {"口", "kou", 3, "品", "pǐn"},
        {"石", "shi", 3, "磊", "lěi"},   {"直", "zhi", 3, "矗", "chù"}, {"牛", "niu", 3, "犇", "bēn"},
        {"马", "ma", 3, "骉", "biāo"}};
    std::string ascii;
    for (unsigned char c : text)
        if (c >= 'a' && c <= 'z')
            ascii += char(c);
    std::vector<Answer> answers;
    for (const auto &entry : entries)
    {
        const bool chinese =
            text.find(std::string(entry.count == 3 ? "三" : "两") + "个" + entry.component) != std::string::npos ||
            text.find(std::string(entry.count == 3 ? "三" : "二") + entry.component) != std::string::npos;
        const std::string prefix = std::string(entry.count == 3 ? "sange" : "liangge") + entry.pinyin;
        if (chinese || ascii.rfind(prefix, 0) == 0)
            answers.push_back({entry.answer, entry.reading, "本地叠字部件表"});
    }
    return answers;
}
Result ParseTaskContent(Task task, const std::string &content, const std::string &precedingText, int limit)
{
    Result result;
    result.task = task;
    if (content.size() > 128 * 1024 || !utf8::is_valid(content.begin(), content.end()))
        throw std::runtime_error("AI 返回内容过长或编码无效");
    auto value = nlohmann::json::parse(content);
    limit = (std::max)(1, (std::min)(10, limit));
    if (task == Task::Completion)
    {
        if (!value.contains("completion") || !value["completion"].is_string())
            throw std::runtime_error("续写结果缺少 completion 字符串");
        result.completion = value["completion"].get<std::string>();
        // A provider may repeat the complete preceding text; never insert it twice.
        if (!precedingText.empty() && result.completion.rfind(precedingText, 0) == 0)
            result.completion.erase(0, precedingText.size());
        if (result.completion.empty() || result.completion.size() > 12000)
            throw std::runtime_error("续写没有新增内容或过长");
        return result;
    }
    const auto &rows = value.at(task == Task::Question ? "answers" : "candidates");
    if (!rows.is_array())
        throw std::runtime_error("AI 答案不是列表");
    std::set<std::string> seen;
    for (const auto &row : rows)
    {
        if (!row.is_object() || !row.contains("text") || !row["text"].is_string())
            continue;
        Answer answer{row["text"].get<std::string>(), row.value("pinyin", std::string()), "AI 模型"};
        const auto key = task == Task::Question ? answer.text + "\x1f" + answer.pinyin : answer.text;
        if (answer.text.empty() || answer.text.size() > 2048 || answer.pinyin.size() > 2048 || !seen.insert(key).second)
            continue;
        if (task == Task::Question &&
            (answer.text == precedingText || answer.text.find("是什么字") != std::string::npos ||
             answer.text.find("怎么读") != std::string::npos))
            continue;
        result.candidates.push_back(std::move(answer));
        if (result.candidates.size() >= size_t(limit))
            break;
    }
    if (result.candidates.empty())
        throw std::runtime_error("AI 没有返回有效答案");
    return result;
}
} // namespace AiAssistant
