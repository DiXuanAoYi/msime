#include "snapshot.h"

#include <cstdlib>
#include <memory>
#include <utility>

namespace msime
{
namespace
{
// 只认接口会产生的那一小撮 JSON：对象、数组、字符串、数字、布尔。
struct Value
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value *find(const std::string &key) const
    {
        for (const auto &member : members)
            if (member.first == key)
                return &member.second;
        return nullptr;
    }
};

class Parser
{
  public:
    explicit Parser(const std::string &text) : text_(text)
    {
    }

    bool parse(Value &out)
    {
        return value(out, 0) && (skip(), position_ == text_.size());
    }

  private:
    void skip()
    {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\n' || text_[position_] == '\r' || text_[position_] == '\t'))
            ++position_;
    }

    bool literal(const char *word)
    {
        std::size_t length = 0;
        while (word[length])
            ++length;
        if (text_.compare(position_, length, word) != 0)
            return false;
        position_ += length;
        return true;
    }

    bool value(Value &out, int depth)
    {
        if (depth > 16)
            return false;
        skip();
        if (position_ >= text_.size())
            return false;
        const char c = text_[position_];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(out, depth);
        if (c == '"')
        {
            out.kind = Value::Kind::String;
            return string(out.string);
        }
        if (c == 't' || c == 'f')
        {
            out.kind = Value::Kind::Bool;
            out.boolean = c == 't';
            return literal(out.boolean ? "true" : "false");
        }
        if (c == 'n')
            return literal("null");
        return number(out);
    }

    bool number(Value &out)
    {
        const char *start = text_.c_str() + position_;
        char *end = nullptr;
        out.number = std::strtod(start, &end);
        if (end == start)
            return false;
        out.kind = Value::Kind::Number;
        position_ += static_cast<std::size_t>(end - start);
        return true;
    }

    bool string(std::string &out)
    {
        ++position_; // 开头的引号
        while (position_ < text_.size())
        {
            const char c = text_[position_++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (position_ >= text_.size())
                return false;
            const char escaped = text_[position_++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                out += escaped;
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                if (position_ + 4 > text_.size())
                    return false;
                const unsigned long code = std::strtoul(text_.substr(position_, 4).c_str(), nullptr, 16);
                position_ += 4;
                appendUtf8(out, static_cast<char32_t>(code));
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    static void appendUtf8(std::string &out, char32_t code)
    {
        if (code < 0x80)
            out += static_cast<char>(code);
        else if (code < 0x800)
        {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool array(Value &out, int depth)
    {
        out.kind = Value::Kind::Array;
        ++position_;
        skip();
        if (position_ < text_.size() && text_[position_] == ']')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1))
                return false;
            skip();
            if (position_ >= text_.size())
                return false;
            const char c = text_[position_++];
            if (c == ']')
                return true;
            if (c != ',')
                return false;
        }
    }

    bool object(Value &out, int depth)
    {
        out.kind = Value::Kind::Object;
        ++position_;
        skip();
        if (position_ < text_.size() && text_[position_] == '}')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            skip();
            if (position_ >= text_.size() || text_[position_] != '"')
                return false;
            std::string key;
            if (!string(key))
                return false;
            skip();
            if (position_ >= text_.size() || text_[position_++] != ':')
                return false;
            Value member;
            if (!value(member, depth + 1))
                return false;
            out.members.emplace_back(std::move(key), std::move(member));
            skip();
            if (position_ >= text_.size())
                return false;
            const char c = text_[position_++];
            if (c == '}')
                return true;
            if (c != ',')
                return false;
        }
    }

    const std::string &text_;
    std::size_t position_ = 0;
};

std::string stringOf(const Value &object, const char *key)
{
    const Value *found = object.find(key);
    return found && found->kind == Value::Kind::String ? found->string : std::string();
}
} // namespace

bool parseSnapshot(const std::string &json, Snapshot &snapshot)
{
    snapshot = Snapshot{};
    Value root;
    if (!Parser(json).parse(root) || root.kind != Value::Kind::Object)
        return false;
    Snapshot result;
    result.scheme = stringOf(root, "scheme");
    result.preedit = stringOf(root, "preedit");
    result.raw = stringOf(root, "raw");
    result.normalized = stringOf(root, "normalized");
    if (const Value *caret = root.find("caret"); caret && caret->kind == Value::Kind::Number)
        result.caret = static_cast<std::size_t>(caret->number);
    if (const Value *list = root.find("candidates"); list && list->kind == Value::Kind::Array)
    {
        for (const Value &item : list->items)
        {
            if (item.kind != Value::Kind::Object)
                return false;
            Candidate candidate;
            candidate.text = stringOf(item, "text");
            candidate.pinyin = stringOf(item, "pinyin");
            if (const Value *weight = item.find("weight"); weight && weight->kind == Value::Kind::Number)
                candidate.weight = static_cast<long long>(weight->number);
            if (const Value *fixed = item.find("fixed"); fixed && fixed->kind == Value::Kind::Number)
                candidate.fixed = static_cast<int>(fixed->number);
            if (const Value *fuzzy = item.find("fuzzy"); fuzzy && fuzzy->kind == Value::Kind::Bool)
                candidate.fuzzy = fuzzy->boolean;
            result.candidates.push_back(std::move(candidate));
        }
    }
    snapshot = std::move(result);
    return true;
}
} // namespace msime
