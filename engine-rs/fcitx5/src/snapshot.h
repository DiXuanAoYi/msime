// 引擎 C 接口 msime_snapshot_json 返回的快照。格式见 engine/capi/msime_capi.h：
// {"scheme":"quanpin","preedit":"...","raw":"...","normalized":"...","caret":3,
//  "candidates":[{"text":"你好","pinyin":"ni'hao","weight":1234,"fixed":0,"fuzzy":false}, ...]}
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace msime
{
struct Candidate
{
    std::string text;
    std::string pinyin;
    long long weight = 0;
    int fixed = 0;
    bool fuzzy = false;
};

struct Snapshot
{
    std::string scheme;
    std::string preedit;
    std::string raw;
    std::string normalized;
    std::size_t caret = 0;
    std::vector<Candidate> candidates;

    bool composing() const
    {
        return !preedit.empty() || !candidates.empty();
    }
};

// 解析失败返回 false，并把 snapshot 留成空的。
bool parseSnapshot(const std::string &json, Snapshot &snapshot);
} // namespace msime
