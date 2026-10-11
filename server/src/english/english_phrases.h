#pragma once
#include <array>
#include <string>
#include <utility>

namespace EnglishIme
{
// Small offline writing aids, authored for this application. Never requests a cloud model.
inline constexpr std::pair<const char *, const char *> kPhrases[] = {
    {"Hello, how are you?", "你好，最近怎么样？"},
    {"Hello, nice to meet you.", "你好，很高兴认识你。"},
    {"Thank you for your help.", "谢谢你的帮助。"},
    {"Thanks for letting me know.", "谢谢你告诉我。"},
    {"Good morning!", "早上好！"},
    {"Good afternoon!", "下午好！"},
    {"Good night, sleep well.", "晚安，睡个好觉。"},
    {"Could you please explain this?", "能请你解释一下吗？"},
    {"Could you send me the details?", "能把详细信息发给我吗？"},
    {"Please let me know when you are available.", "请告诉我你什么时候有空。"},
    {"Please find the attached document.", "请查收附件。"},
    {"Sorry for the late reply.", "抱歉回复晚了。"},
    {"Sorry, I did not understand that.", "抱歉，我没理解那句话。"},
    {"Sounds good to me.", "我觉得不错。"},
    {"Looking forward to hearing from you.", "期待你的回复。"},
    {"Have a great day!", "祝你今天愉快！"},
    {"Happy to help!", "很乐意帮忙！"},
    {"Let me check and get back to you.", "我核实后回复你。"},
    {"No problem at all.", "完全没问题。"},
    {"Nice to hear from you.", "很高兴收到你的消息。"},
    {"See you tomorrow!", "明天见！"},
    {"Best regards,", "祝好，"},
    {"How can I help you?", "有什么我可以帮你的？"},
    {"Would you like to join us?", "你愿意加入我们吗？"},
    {"Welcome back!", "欢迎回来！"},
    {"Congratulations on your success!", "祝贺你取得成功！"},
    {"Take your time.", "慢慢来，不着急。"},
    {"Everything is going well.", "一切顺利。"},
    {"Absolutely, I agree.", "完全同意。"},
    {"Unfortunately, I am not available then.", "很遗憾，我那时没空。"}};
inline std::string PhraseGloss(const std::string &word)
{
    for (const auto &[phrase, gloss] : kPhrases)
        if (word == phrase)
            return gloss;
    return {};
}
} // namespace EnglishIme
