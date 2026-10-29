#include "driver.h"
#include "../../../engine/capi/msime_capi.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace msime
{
bool Driver::seedUserData(const std::string &resourcesDir, const std::string &userDataDir, std::string &error)
{
    std::error_code failure;
    fs::create_directories(userDataDir, failure);
    if (failure)
    {
        error = "无法创建用户目录 " + userDataDir + "：" + failure.message();
        return false;
    }
    for (const char *name : {"msime.db", "english.db"})
    {
        const fs::path target = fs::path(userDataDir) / name;
        if (fs::exists(target))
            continue;
        fs::copy_file(fs::path(resourcesDir) / name, target, failure);
        if (failure)
        {
            error = std::string("无法拷贝 ") + name + "：" + failure.message();
            return false;
        }
    }
    return true;
}

Driver::Driver(const Options &options)
{
    if (!seedUserData(options.resourcesDir, options.userDataDir, error_))
        return;
    std::error_code failure;
    fs::create_directories(options.cacheDir, failure);

    msime_options raw = {0};
    raw.resources_dir = options.resourcesDir.c_str();
    raw.user_data_dir = options.userDataDir.c_str();
    raw.cache_dir = options.cacheDir.c_str();
    raw.scheme = options.scheme.c_str();
    raw.helpcode = options.helpcode ? 1 : 0;
    raw.learning = options.learning ? 1 : 0;
    raw.fuzzy_rules = options.fuzzyRules;
    raw.autocorrect_types = options.autocorrectTypes;
    char message[256] = {0};
    session_ = msime_session_create(&raw, message, sizeof(message));
    if (!session_)
        error_ = message;
}

Driver::~Driver()
{
    if (session_)
        msime_session_destroy(session_);
}

Outcome Driver::record(int handled)
{
    Outcome outcome;
    outcome.handled = handled != 0;
    outcome.commit = msime_commit_text(session_);
    return outcome;
}

Outcome Driver::apply(const Action &action, std::size_t pageStart, std::size_t highlighted)
{
    if (!session_)
        return {};
    switch (action.kind)
    {
    case ActionKind::Character:
        return record(msime_character(session_, action.value, action.shiftOnly ? 1 : 0));
    case ActionKind::Punctuation:
        return record(msime_punctuation(session_, action.value));
    case ActionKind::Command:
        return record(msime_command(session_, static_cast<int>(action.command)));
    case ActionKind::CandidateDigit: {
        const std::size_t index = pageStart + static_cast<std::size_t>(action.value) - 1;
        Outcome outcome = record(msime_select(session_, index));
        outcome.handled = true; // 页里没有这个序号也要吞掉数字键
        return outcome;
    }
    case ActionKind::CommitHighlighted: {
        Outcome outcome = record(msime_select(session_, highlighted));
        outcome.handled = true;
        return outcome;
    }
    case ActionKind::PageUp:
    case ActionKind::PageDown:
    case ActionKind::Swallow: {
        Outcome outcome;
        outcome.handled = true;
        return outcome;
    }
    case ActionKind::PassThrough:
        break;
    }
    return {};
}

Outcome Driver::select(std::size_t index)
{
    if (!session_)
        return {};
    return record(msime_select(session_, index));
}

void Driver::reset()
{
    if (session_)
        msime_command(session_, MSIME_CMD_CANCEL);
}

Snapshot Driver::snapshot()
{
    Snapshot result;
    if (session_)
        parseSnapshot(msime_snapshot_json(session_), result);
    return result;
}
} // namespace msime
