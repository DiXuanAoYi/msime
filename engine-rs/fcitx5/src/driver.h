// 对引擎 C 接口的一层薄封装：持有会话句柄，把按键动作翻译成接口调用，读回上屏文字和快照。不依赖 fcitx5。
#pragma once
#include "key_map.h"
#include "snapshot.h"

#include <cstddef>
#include <string>

struct msime_session;

namespace msime
{
struct Options
{
    std::string resourcesDir; // 只读资源：词库、模型、辅助码表
    std::string userDataDir;  // 用户词库与学习结果；接口把词库也放在这里，首次使用从资源目录拷一份
    std::string cacheDir;
    std::string scheme = "quanpin";
    bool helpcode = true;
    bool learning = true;
    unsigned fuzzyRules = 0;
    unsigned autocorrectTypes = 0;
};

struct Outcome
{
    bool handled = false;
    std::string commit;
};

class Driver
{
  public:
    explicit Driver(const Options &options);
    ~Driver();
    Driver(const Driver &) = delete;
    Driver &operator=(const Driver &) = delete;

    bool ok() const
    {
        return session_ != nullptr;
    }
    const std::string &error() const
    {
        return error_;
    }

    // pageStart：当前页第一个候选的总序号；highlighted：高亮候选的总序号。数字键和空格按它们换算。
    Outcome apply(const Action &action, std::size_t pageStart = 0, std::size_t highlighted = 0);
    Outcome select(std::size_t index);
    void reset();
    Snapshot snapshot();

    // 词库在用户目录里才能被学习改写；没有就从资源目录拷过来。
    static bool seedUserData(const std::string &resourcesDir, const std::string &userDataDir, std::string &error);

  private:
    Outcome record(int handled);

    msime_session *session_ = nullptr;
    std::string error_;
};
} // namespace msime
