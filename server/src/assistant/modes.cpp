#include "modes.h"
namespace ReplyStudio
{
const std::vector<Mode> &Modes()
{
    static const std::vector<Mode> modes = {
        {R"MODE(spokesperson)MODE", R"MODE(嘴替)MODE", R"MODE(把想说又说不清的话说到点上)MODE",
         R"MODE(替用户把真实意思说清楚，自然直接，保留立场，不代替用户作出承诺。)MODE"},
        {R"MODE(formal)MODE", R"MODE(正式得体)MODE", R"MODE(用词规范，先结论后细节)MODE",
         R"MODE(语气正式得体，先给结论再补细节，称呼和措辞适合领导、客户等正式场合；不夸张、不卖惨，不替用户作出没有依据的承诺。)MODE"},
        {R"MODE(conditional_yes)MODE", R"MODE(答应但设条件)MODE", R"MODE(接下来，同时讲清前提和边界)MODE",
         R"MODE(先明确答应，再说清前提、时间和边界（例如需要对方提供的资料、能够完成的程度）；条件要具体、合理，不用条件当借口，不编造用户没有给出的限制。)MODE"},
        {R"MODE(humor)MODE", R"MODE(幽默接话)MODE", R"MODE(轻松接梗，化解尴尬)MODE",
         R"MODE(用轻松幽默的方式接话，适度自嘲或调侃情境，不冒犯对方，不涉及身份、外貌、疾病；对方明显严肃或生气时降低玩笑，优先把事情说清楚。)MODE"},
        {R"MODE(rewrite_styles)MODE", R"MODE(帮我改)MODE", R"MODE(把草稿改成几种不同说法)MODE",
         R"MODE(incoming 是用户自己打的草稿。必须恰好输出 5 条，label 依次固定为：更礼貌、更简短、更坚定、更热情、译成英文。保留原意和事实，不增加用户没说过的承诺；“译成英文”只翻译不润色。)MODE"},
        {R"MODE(polish)MODE", R"MODE(润色)MODE", R"MODE(保留原意，让表达顺畅得体)MODE",
         R"MODE(修正口语、重复和语序，保留事实与语气，不扩写不存在的经历。)MODE"},
        {R"MODE(delivery_review)MODE", R"MODE(外卖好评)MODE", R"MODE(根据真实体验写自然评价)MODE",
         R"MODE(根据用户提供的口味、分量、包装、配送体验写自然的外卖好评；没有提到的体验不编造，不虚构订单或评分。)MODE"},
        {R"MODE(love_advisor)MODE", R"MODE(恋爱军师)MODE", R"MODE(看懂语气，给出有分寸的回应)MODE",
         R"MODE(结合对话分析可能的沟通需求，给出尊重双方意愿的回复；不把猜测当事实，不教人操控、试探或逼迫对方。)MODE"},
        {R"MODE(high_eq)MODE", R"MODE(高情商回复)MODE", R"MODE(有温度，也守住自己的边界)MODE",
         R"MODE(先回应对方感受，再清楚表达自己的需要，友善而不讨好，避免过度承诺。)MODE"},
        {R"MODE(sharp_tongue)MODE", R"MODE(淬毒之嘴)MODE", R"MODE(犀利吐槽，句短有梗)MODE",
         R"MODE(用机智、反讽、幽默拆解对方言论；可以犀利，但针对言行而非身份、外貌、疾病；不威胁、不曝光隐私。)MODE"},
        {R"MODE(comeback)MODE", R"MODE(回怼王)MODE", R"MODE(抓住逻辑，坚定回击)MODE",
         R"MODE(指出矛盾、反问关键事实、清楚划定边界；简短有力，不编造黑料，不发起围攻或暴力威胁。)MODE"},
        {R"MODE(flirt)MODE", R"MODE(情话撩人)MODE", R"MODE(自然不油腻的暧昧表达)MODE",
         R"MODE(依据已有关系写轻松、真诚、不油腻的情话，留给对方拒绝和不回复的空间，不臆造亲密经历。)MODE"},
        {R"MODE(gentle_no)MODE", R"MODE(体面拒绝)MODE", R"MODE(拒绝清楚，不绕弯子)MODE",
         R"MODE(简短表达感谢与拒绝，理由仅用用户提供的事实，必要时给出可行替代方案，不留下虚假期待。)MODE"},
        {R"MODE(follow_up)MODE", R"MODE(催进度)MODE", R"MODE(把催促说得明确又礼貌)MODE",
         R"MODE(说明事项、所需结果和用户提供的时间点，礼貌询问进度；没给截止时间就不要编造。)MODE"},
        {R"MODE(repair)MODE", R"MODE(道歉修复)MODE", R"MODE(承担责任，提出具体补救)MODE",
         R"MODE(承认已知影响和自己的责任，避免甩锅与空泛道歉，提出用户确实能做到的补救，不替用户承诺。)MODE"},
        {R"MODE(work_update)MODE", R"MODE(工作汇报)MODE", R"MODE(结论、进展、风险、下一步)MODE",
         R"MODE(先讲结论，再分进展、风险、下一步，保留真实数字与日期；未知事项明确待确认。)MODE"},
        {R"MODE(social_post)MODE", R"MODE(朋友圈文案)MODE", R"MODE(有画面感，保留个人口吻)MODE",
         R"MODE(根据真实素材写短文案，有画面感和轻松语气，不炫耀虚构经历；少用套话和堆砌表情。)MODE"},
        {R"MODE(meeting_notes)MODE", R"MODE(会议纪要)MODE", R"MODE(提炼结论和待办)MODE",
         R"MODE(整理背景、结论、分歧、行动项，责任人和日期仅据原文填写，未知写待确认。)MODE"},
        {R"MODE(ai_prompt)MODE", R"MODE(AI 提示词)MODE", R"MODE(把口述需求整理成清晰指令)MODE",
         R"MODE(将用户需求整理为目标、已知背景、约束和输出要求；不擅自添加事实，缺少的关键信息列为待补充。)MODE"},
    };
    return modes;
}
const Mode *FindMode(std::string_view id)
{
    for (const auto &mode : Modes())
        if (mode.id == id)
            return &mode;
    return nullptr;
}
} // namespace ReplyStudio
