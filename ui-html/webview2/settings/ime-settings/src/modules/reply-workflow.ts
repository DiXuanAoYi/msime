export type WritingTask = "reply" | "rewrite" | "write";
export const quickModes = ["high_eq", "love_advisor", "comeback", "sharp_tongue", "flirt", "gentle_no"];
export const scenarios: Record<string, { incoming: string; intent: string; relationship: string }> = {
  high_eq: { incoming: "最近怎么都不主动找我了？", intent: "最近忙，想解释一下，也想约个时间好好聊", relationship: "好朋友" },
  love_advisor: { incoming: "你忙你的吧，不用管我。", intent: "想关心对方，不想一上来讲道理，也不想乱承诺", relationship: "恋人" },
  comeback: { incoming: "这么简单的事你都做不好？", intent: "指出问题可以谈，但不接受贬低；语气坚定", relationship: "同事" },
  sharp_tongue: { incoming: "我这是为你好，你就别有自己的想法了。", intent: "用一句机智的话回应，守住边界", relationship: "熟人" },
  flirt: { incoming: "今天的晚霞好漂亮。", intent: "自然接话，表达想一起看的意思，不要油腻", relationship: "彼此有好感，还未确认关系" },
  gentle_no: { incoming: "今晚能帮我把这份报告也做了吗？", intent: "今晚没时间，不想接下这项工作；礼貌拒绝", relationship: "同事" },
  delivery_review: { incoming: "面条分量足，包装没有洒，口味偏清淡。", intent: "根据实际体验写一段自然好评", relationship: "" },
};

export function taskDefaultMode(task: WritingTask): string {
  return task === "rewrite" ? "polish" : task === "write" ? "delivery_review" : "high_eq";
}
