#include "capi/msime_capi.h"
#include "include/metasequoia/session.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace
{
constexpr int kAbiVersion = 2;

void WriteError(char *buffer, size_t length, const std::string &message)
{
    if (buffer == nullptr || length == 0)
        return;
    std::snprintf(buffer, length, "%s", message.c_str());
}

void AppendJsonString(std::string &out, const std::string &text)
{
    out += '"';
    for (const unsigned char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            }
            else
                out += static_cast<char>(c);
        }
    }
    out += '"';
}

const char *SchemeName(SchemeType scheme)
{
    switch (scheme)
    {
    case SchemeType::Shuangpin:
        return "shuangpin";
    case SchemeType::Wubi:
        return "wubi";
    case SchemeType::JapaneseRomaji:
        return "japanese";
    case SchemeType::Quanpin:
    default:
        return "quanpin";
    }
}

bool ParseScheme(const char *name, SchemeType &scheme)
{
    if (name == nullptr || *name == 0 || std::strcmp(name, "quanpin") == 0)
        scheme = SchemeType::Quanpin;
    else if (std::strcmp(name, "shuangpin") == 0)
        scheme = SchemeType::Shuangpin;
    else if (std::strcmp(name, "wubi") == 0)
        scheme = SchemeType::Wubi;
    else if (std::strcmp(name, "japanese") == 0)
        scheme = SchemeType::JapaneseRomaji;
    else
        return false;
    return true;
}
} // namespace

struct msime_session
{
    std::unique_ptr<metasequoia::Session> session;
    std::string commit;
    std::string snapshot;

    int Record(const metasequoia::KeyResult &result)
    {
        commit = result.commit.value_or(std::string());
        return result.handled ? 1 : 0;
    }
};

extern "C"
{
    int msime_abi_version(void)
    {
        return kAbiVersion;
    }

    msime_session *msime_session_create(const msime_options *options, char *error, size_t error_len)
    {
        try
        {
            if (options == nullptr || options->resources_dir == nullptr || options->user_data_dir == nullptr ||
                options->cache_dir == nullptr)
            {
                WriteError(error, error_len, "resources_dir、user_data_dir、cache_dir 都必须提供");
                return nullptr;
            }
            metasequoia::SessionOptions session_options;
            if (!ParseScheme(options->scheme, session_options.scheme))
            {
                WriteError(error, error_len, "不认识的输入方案");
                return nullptr;
            }
            session_options.paths.resources = std::filesystem::u8path(options->resources_dir);
            session_options.paths.user_data = std::filesystem::u8path(options->user_data_dir);
            session_options.paths.cache = std::filesystem::u8path(options->cache_dir);
            session_options.paths.dictionaries = session_options.paths.user_data;
            session_options.helpcode = options->helpcode != 0;
            session_options.learning = options->learning != 0;
            session_options.fuzzy_pinyin.rules = options->fuzzy_rules;
            session_options.autocorrect_types = options->autocorrect_types;
            auto handle = std::make_unique<msime_session>();
            handle->session = std::make_unique<metasequoia::Session>(std::move(session_options));
            return handle.release();
        }
        catch (const std::exception &failure)
        {
            WriteError(error, error_len, failure.what());
        }
        catch (...)
        {
            WriteError(error, error_len, "创建会话时发生未知错误");
        }
        return nullptr;
    }

    void msime_session_destroy(msime_session *session)
    {
        delete session;
    }

    int msime_character(msime_session *s, char value, int shift_only)
    {
        return s ? s->Record(s->session->character(value, shift_only != 0)) : 0;
    }

    int msime_command(msime_session *s, int command)
    {
        if (!s || command < MSIME_CMD_BACKSPACE || command > MSIME_CMD_DELETE_FORWARD)
            return 0;
        return s->Record(s->session->command(static_cast<metasequoia::Command>(command)));
    }

    int msime_candidate_key(msime_session *s, char value)
    {
        return s ? s->Record(s->session->candidate_key(value)) : 0;
    }

    int msime_punctuation(msime_session *s, char value)
    {
        return s ? s->Record(s->session->punctuation(value)) : 0;
    }

    int msime_select(msime_session *s, size_t index)
    {
        return s ? s->Record(s->session->select(index)) : 0;
    }

    int msime_finish(msime_session *s)
    {
        return s ? s->Record(s->session->finish()) : 0;
    }

    const char *msime_commit_text(const msime_session *s)
    {
        return s ? s->commit.c_str() : "";
    }

    const char *msime_snapshot_json(msime_session *s)
    {
        if (!s)
            return "{}";
        const auto snapshot = s->session->snapshot();
        std::string &out = s->snapshot;
        out = "{\"scheme\":\"";
        out += SchemeName(snapshot.scheme);
        out += "\",\"preedit\":";
        AppendJsonString(out, snapshot.preedit);
        out += ",\"raw\":";
        AppendJsonString(out, snapshot.raw_segmentation);
        out += ",\"normalized\":";
        AppendJsonString(out, snapshot.normalized_segmentation);
        out += ",\"caret\":" + std::to_string(snapshot.caret_position) + ",\"candidates\":[";
        bool first = true;
        for (const auto &item : snapshot.candidates)
        {
            if (!first)
                out += ',';
            first = false;
            out += "{\"text\":";
            AppendJsonString(out, item.word);
            out += ",\"pinyin\":";
            AppendJsonString(out, item.pinyin);
            out += ",\"weight\":" + std::to_string(item.weight) + ",\"fixed\":" + std::to_string(item.fixed_position) +
                   ",\"fuzzy\":" + (item.fuzzy ? "true" : "false") + "}";
        }
        out += "]}";
        return out.c_str();
    }
}
