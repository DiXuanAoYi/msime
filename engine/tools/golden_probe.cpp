// 黄金样本探针：把一份“输入串”清单喂给引擎，按行输出每条的候选，用来当别的实现（比如 Rust 核心）的行为规格。
//
// 用法：msime_golden_probe <资源目录> <用户数据目录> <缓存目录> <样本文件> [输出文件]
//
// 样本文件每行一条：`方案<TAB>按键[<TAB>选项...]`，例如 `quanpin<TAB>nihao`；空行和 # 开头的行跳过。
// 选项是 `名=值`：`fuzzy=模糊音位掩码`、`autocorrect=纠错类型位掩码`（十进制），不写就是关闭。
// 每条先逐个按键输入，再取一次快照，输出一行 JSON：
//   {"scheme":"quanpin","keys":"nihao","snapshot":{...msime_snapshot_json...}}
// 不选词、不提交，所以不会改动用户词库；`learning` 固定关闭，保证重复运行结果一致。
#include "capi/msime_capi.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
std::string JsonString(const std::string &text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += std::string("\\") + static_cast<char>(c);
        else if (c < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::cerr << "用法：msime_golden_probe <资源目录> <用户数据目录> <缓存目录> <样本文件> [输出文件]\n";
        return 2;
    }
    std::ifstream corpus(argv[4]);
    if (!corpus)
    {
        std::cerr << "打不开样本文件：" << argv[4] << "\n";
        return 2;
    }
    std::ofstream file;
    if (argc > 5)
        file.open(argv[5], std::ios::binary);
    std::ostream &out = argc > 5 ? static_cast<std::ostream &>(file) : std::cout;

    std::string line;
    int failures = 0;
    while (std::getline(corpus, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
        {
            std::cerr << "样本缺少制表符：" << line << "\n";
            ++failures;
            continue;
        }
        const std::string scheme = line.substr(0, tab);
        std::string keys = line.substr(tab + 1);
        unsigned fuzzy = 0, autocorrect = 0;
        std::string option_text;
        if (const auto second = keys.find('\t'); second != std::string::npos)
        {
            option_text = keys.substr(second + 1);
            keys.resize(second);
        }
        for (std::size_t at = 0; at < option_text.size();)
        {
            const auto end = option_text.find('\t', at);
            const std::string item = option_text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (item.rfind("fuzzy=", 0) == 0)
                fuzzy = static_cast<unsigned>(std::stoul(item.substr(6)));
            else if (item.rfind("autocorrect=", 0) == 0)
                autocorrect = static_cast<unsigned>(std::stoul(item.substr(12)));
            at = end == std::string::npos ? option_text.size() : end + 1;
        }
        msime_options options{};
        options.resources_dir = argv[1];
        options.user_data_dir = argv[2];
        options.cache_dir = argv[3];
        options.scheme = scheme.c_str();
        options.helpcode = 1;
        options.fuzzy_rules = fuzzy;
        options.autocorrect_types = autocorrect;
        char error[256] = {};
        msime_session *session = msime_session_create(&options, error, sizeof(error));
        if (!session)
        {
            std::cerr << "创建会话失败（" << scheme << "）：" << error << "\n";
            ++failures;
            continue;
        }
        for (const char key : keys)
            msime_character(session, key, 0);
        out << "{\"scheme\":" << JsonString(scheme) << ",\"keys\":" << JsonString(keys) << ",\"fuzzy\":" << fuzzy
            << ",\"autocorrect\":" << autocorrect << ",\"snapshot\":" << msime_snapshot_json(session) << "}\n";
        msime_session_destroy(session);
    }
    return failures == 0 ? 0 : 1;
}
