// C 接口探针：真实的 msime_capi（C++ 版）吃随机操作序列——字符、命令、选词键、标点、选词、整句上屏——
// 全拼、双拼、五笔、日语各一批，每步导出返回值、上屏文字和快照 JSON 原文。
//
// 接口把 dictionaries 当成 user_data，所以探针把 msime.db、english.db 拷进用户目录；用户目录在序列之间不清，
// 学习结果照常累积（Rust 测试从同一份拷贝开始、走同样的序列）。字符一律不带 shift_only，不触发读系统时钟的日期时间模式。
//
// 用法：msime_capi_probe <资源目录> <工作目录> <输出文件>
#include "capi/msime_capi.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace
{
struct Lcg
{
    std::uint64_t state;
    std::uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n)
    {
        return next() % n;
    }
};

std::string quote(const std::string &text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += std::string("\\") + static_cast<char>(c);
        else if (c < 0x20)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string hash_text(const std::string &text)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char c : text)
    {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

// 快照 JSON 太长，基线只留长度和哈希；初始快照很短，留原文。
std::string snapshot_digest(const std::string &snapshot)
{
    return "[" + std::to_string(snapshot.size()) + ",\"" + hash_text(snapshot) + "\"]";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_capi_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::remove_all(work);
    const fs::path user = work / "user";
    fs::create_directories(user);
    fs::copy_file(resources / "msime.db", user / "msime.db");
    fs::copy_file(resources / "english.db", user / "english.db");
    std::ofstream out(argv[3], std::ios::binary);
    Lcg rng{0x2b7e1516ULL};

    const std::string resources_text = resources.u8string();
    const std::string user_text = user.u8string();
    const std::string cache_text = (work / "cache").u8string();
    static const char *schemes[] = {"quanpin", "shuangpin", "wubi", "japanese"};
    int sequences = 0, steps = 0, handled = 0, committed = 0, candidates = 0;
    for (int round = 0; round < 200; ++round)
    {
        msime_options options = {0};
        options.resources_dir = resources_text.c_str();
        options.user_data_dir = user_text.c_str();
        options.cache_dir = cache_text.c_str();
        options.scheme = schemes[round % 4];
        options.helpcode = rng.below(2);
        options.learning = rng.below(5) != 0;
        options.fuzzy_rules = rng.below(5) == 0 ? rng.below(1u << 11) : 0u;
        options.autocorrect_types = rng.below(4) == 0 ? 3u : 0u;
        char error[256] = {0};
        msime_session *session = msime_session_create(&options, error, sizeof(error));
        if (!session)
        {
            std::cerr << "创建失败：" << error << "\n";
            return 1;
        }
        out << "{\"round\":" << round << ",\"scheme\":" << quote(options.scheme) << ",\"help\":" << options.helpcode << ",\"learn\":" << options.learning << ",\"fuzzy\":" << options.fuzzy_rules
            << ",\"ac\":" << options.autocorrect_types << ",\"init\":" << quote(msime_snapshot_json(session)) << ",\"ops\":[";
        const std::uint32_t length = 8 + rng.below(30);
        for (std::uint32_t i = 0; i < length; ++i)
        {
            if (i)
                out << ",";
            const std::uint32_t pick = rng.below(100);
            int rc = 0;
            if (pick < 55)
            {
                static const char letters[] = "abcdefghijklmnopqrstuvwxyzabcdeghilmnorsuyz'''ABHXZ";
                const char c = letters[rng.below(sizeof(letters) - 1)];
                rc = msime_character(session, c, 0);
                out << "{\"op\":\"char\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 68)
            {
                const int command = static_cast<int>(rng.below(10)); /* 9 越界，接口返回 0 */
                rc = msime_command(session, command);
                out << "{\"op\":\"command\",\"c\":" << command;
            }
            else if (pick < 76)
            {
                const char c = static_cast<char>('0' + rng.below(11));
                rc = msime_candidate_key(session, c);
                out << "{\"op\":\"candidate_key\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 82)
            {
                static const char marks[] = ",.?!;:()[]\\`$^_\"'<>a";
                const char c = marks[rng.below(sizeof(marks) - 1)];
                rc = msime_punctuation(session, c);
                out << "{\"op\":\"punctuation\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 93)
            {
                const std::size_t index = rng.below(8);
                rc = msime_select(session, index);
                out << "{\"op\":\"select\",\"i\":" << index;
            }
            else
            {
                rc = msime_finish(session);
                out << "{\"op\":\"finish\"";
            }
            const std::string commit = msime_commit_text(session);
            const std::string snapshot = msime_snapshot_json(session);
            out << ",\"rc\":" << rc << ",\"commit\":" << quote(commit) << ",\"snap\":" << snapshot_digest(snapshot) << "}";
            ++steps;
            handled += rc;
            committed += !commit.empty();
            candidates += snapshot.find("\"text\"") != std::string::npos;
        }
        out << "]}\n";
        msime_session_destroy(session);
        ++sequences;
    }
    std::cerr << "C 接口：" << sequences << " 个序列 " << steps << " 步，处理 " << handled << "，有上屏 " << committed << "，有候选 " << candidates << "\n";
    return 0;
}
