/**
 * 工作台的三栏外观：左边会话列表、中间聊天气泡、右边帮我回/改/写。
 * 所有数据仍在 reply-studio.ts 的原有输入框里，这里只把它们读出来重新画，再把点击转回去。
 */

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T | null;

export interface Bubble {
  who: "me" | "other" | "note";
  name: string;
  text: string;
}

/** Turns the saved record into chat bubbles. Understands "名字：内容" lines and "[时间] 名字：\n内容" blocks. */
export function parseChat(raw: string, myNames: string[] = ["我", "me", "Me"]): Bubble[] {
  const out: Bubble[] = [];
  const lines = raw.split(/\r?\n/);
  const push = (name: string, text: string) => {
    const mine = myNames.includes(name);
    out.push({ who: mine ? "me" : "other", name, text: text.trim() });
  };
  let pending: { name: string; text: string } | null = null;
  const flush = () => {
    if (pending && pending.text.trim()) push(pending.name, pending.text);
    pending = null;
  };
  for (const line of lines) {
    const block = line.match(/^\[[^\]]*\]\s*([^：:\n]{1,20})[：:]\s*$/);
    const inline = line.match(/^(?:\[[^\]]*\]\s*)?([^：:\s\[][^：:]{0,19})[：:]\s*(.*)$/);
    if (block) {
      flush();
      pending = { name: block[1].trim(), text: "" };
    } else if (inline && inline[2] !== undefined && !(pending && !line.trim())) {
      flush();
      pending = { name: inline[1].trim(), text: inline[2] };
    } else if (pending) {
      pending.text += (pending.text ? "\n" : "") + line;
    } else if (line.trim()) {
      out.push({ who: "note", name: "", text: line.trim() });
    }
  }
  flush();
  return out.filter((bubble) => bubble.text);
}

export function setupReplyBench(): void {
  const root = $("replyStudio");
  if (!root || root.dataset.bench) return;
  root.dataset.bench = "1";
  const sessions = $<HTMLSelectElement>("rsSessions")!;
  const list = $("wbSessions")!;
  const msgs = $("wbMsgs")!;
  const paste = $("wbPaste")!;
  const pasteBtn = $<HTMLButtonElement>("wbPasteBtn")!;
  const context = $<HTMLTextAreaElement>("rsContext")!;
  const title = $<HTMLInputElement>("rsTitle")!;
  const task = $<HTMLSelectElement>("rsTask")!;

  const paintSessions = () => {
    list.replaceChildren();
    [...sessions.options].forEach((option) => {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "wb-s";
      row.setAttribute("aria-pressed", String(option.value === sessions.value));
      const [kind, ...rest] = option.value ? option.textContent!.split(" · ") : ["", option.textContent!];
      const name = document.createElement("b");
      name.textContent = option.value ? rest.join(" · ") : "临时会话";
      const sub = document.createElement("small");
      sub.textContent = option.value ? (kind === "群" ? "群聊" : "联系人") : "不保存，关闭即清";
      row.append(name, sub);
      row.onclick = () => {
        sessions.value = option.value;
        sessions.dispatchEvent(new Event("change"));
      };
      list.append(row);
    });
  };

  const paintChat = () => {
    msgs.replaceChildren();
    const bubbles = parseChat(context.value);
    if (!bubbles.length) {
      const empty = document.createElement("p");
      empty.className = "wb-empty";
      empty.textContent = context.value.trim() ? context.value.trim() : "还没有聊天记录。点“粘贴聊天记录”，或在聊天里用“帮我回”后自动记下。";
      msgs.append(empty);
    }
    for (const bubble of bubbles) {
      const item = document.createElement("div");
      item.className = `wb-m ${bubble.who}`;
      if (bubble.name && bubble.who === "other") {
        const name = document.createElement("small");
        name.textContent = bubble.name;
        item.append(name);
      }
      const text = document.createElement("span");
      text.textContent = bubble.text;
      item.append(text);
      msgs.append(item);
    }
    msgs.scrollTop = msgs.scrollHeight;
    $("wbTitle")!.textContent = title.value.trim() || "临时会话";
  };

  const paintMode = () =>
    root.querySelectorAll<HTMLElement>("#wbMode button").forEach((button) =>
      button.setAttribute("aria-pressed", String(button.dataset.wbm === task.value)),
    );

  new MutationObserver(paintSessions).observe(sessions, { childList: true });
  sessions.addEventListener("change", () => window.setTimeout(paintSessions, 0));
  for (const field of [context, title]) field.addEventListener("input", paintChat);
  // reply-studio.ts writes these programmatically (loading a session, 追加, 导入), which fires no input event.
  const watch = window.setInterval(() => {
    if (!document.body.contains(root)) return window.clearInterval(watch);
    if (context.value !== (msgs.dataset.seen ?? "") || title.value !== (msgs.dataset.title ?? "")) {
      msgs.dataset.seen = context.value;
      msgs.dataset.title = title.value;
      paintChat();
      paintSessions();
    }
    paintMode();
  }, 400);

  pasteBtn.onclick = () => {
    paste.hidden = !paste.hidden;
    pasteBtn.textContent = paste.hidden ? "粘贴聊天记录" : "收起编辑";
  };
  $("wbPasteDone")!.onclick = () => {
    paste.hidden = true;
    pasteBtn.textContent = "粘贴聊天记录";
    paintChat();
  };
  root.querySelectorAll<HTMLElement>("#wbMode button").forEach((button) =>
    button.addEventListener("click", () => {
      task.value = button.dataset.wbm!;
      task.dispatchEvent(new Event("change"));
      paintMode();
    }),
  );
  task.addEventListener("change", paintMode);
  paintSessions();
  paintChat();
  paintMode();
}
