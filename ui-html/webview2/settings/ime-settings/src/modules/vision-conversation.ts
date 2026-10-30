import { personalAction } from "./personal-library";

/**
 * 对话翻译的记录：翻译本身仍由 vision.ts 完成（两个输入框 + 翻译按钮），这里只负责把每一句
 * 译完的话记成聊天气泡，并提供 Ctrl+Enter、复制全部和清空。记录只放在内存里，关闭窗口即丢。
 */

export interface Turn {
  side: "A" | "B";
  source: string;
  translation: string;
  from: string;
  to: string;
}

export function formatTranscript(turns: Turn[]): string {
  return turns
    .map((turn) => `${turn.side === "A" ? "我" : "对方"}：${turn.source}\n${turn.translation}`)
    .join("\n\n");
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

export function setupConversation(): void {
  const thread = $("conversationThread");
  if (!thread || thread.dataset.ready) return;
  thread.dataset.ready = "1";
  const turns: Turn[] = [];
  const label = (id: string) => {
    const select = $<HTMLSelectElement>(id);
    return select.selectedOptions[0]?.textContent ?? select.value;
  };

  const paint = () => {
    thread.replaceChildren();
    if (!turns.length) {
      const empty = document.createElement("p");
      empty.className = "conv-empty";
      empty.textContent = "还没有对话。在下面输入一句话并翻译，译文会和原文一起记在这里。";
      thread.append(empty);
    }
    for (const turn of turns) {
      const bubble = document.createElement("div");
      bubble.className = `conv-b ${turn.side === "A" ? "me" : "other"}`;
      const tag = document.createElement("small");
      tag.textContent = `${turn.side === "A" ? "我" : "对方"} · ${turn.from} → ${turn.to}`;
      const source = document.createElement("span");
      source.className = "conv-src";
      source.textContent = turn.source;
      const translation = document.createElement("span");
      translation.className = "conv-tr";
      translation.textContent = turn.translation;
      const copy = document.createElement("button");
      copy.type = "button";
      copy.textContent = "复制译文";
      copy.onclick = () => personalAction("copy", { text: turn.translation });
      bubble.append(tag, source, translation, copy);
      thread.append(bubble);
    }
    thread.scrollTop = thread.scrollHeight;
  };

  for (const side of ["A", "B"] as const) {
    const text = $<HTMLTextAreaElement>(`conversationText${side}`);
    const result = $<HTMLTextAreaElement>(`conversationResult${side}`);
    result.addEventListener("change", () => {
      const source = text.value.trim();
      const translation = result.value.trim();
      if (!source || !translation) return;
      turns.push({
        side,
        source,
        translation,
        from: label(`conversationLang${side}`),
        to: label(`conversationLang${side === "A" ? "B" : "A"}`),
      });
      text.value = "";
      paint();
    });
    text.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        $(`conversationTranslate${side}`).click();
      }
    });
  }
  $("conversationCopyAll").onclick = () => {
    if (turns.length) personalAction("copy", { text: formatTranscript(turns) });
  };
  $("conversationClear").onclick = () => {
    turns.length = 0;
    for (const id of ["conversationTextA", "conversationTextB", "conversationResultA", "conversationResultB"])
      $<HTMLTextAreaElement>(id).value = "";
    paint();
  };
  paint();
}
