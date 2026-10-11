import { onHostMessage } from "../utils/host-messages";
import {
  serializeHostMessage,
  type ClientMessage,
  type ServerMessage,
} from "../../../../shared/messages";
type Request = Extract<ClientMessage, { type: "chatRequest" }>["data"];
type Response = Extract<ServerMessage, { type: "chatResponse" }>;
type Binding = NonNullable<Request["bindings"]>[number];
type WindowRow = NonNullable<Response["windows"]>[number];
type Session = NonNullable<Response["session"]>;
type EventRow = NonNullable<Response["events"]>[number];
const platformNames: Record<string, string> = {
  wechat: "微信",
  qq: "QQ",
  wecom: "企业微信",
  dingtalk: "钉钉",
  feishu: "飞书",
};
const byteLength = (s: string) => new TextEncoder().encode(s).length;
export function chatContext(rows: Session["messages"], limit = 46000): string {
  const lines: string[] = [];
  let size = 0;
  for (let i = rows.length - 1; i >= 0; --i) {
    const m = rows[i];
    const line = `[${m.time || "原始时间未提供"}] ${m.self ? "我" : m.speaker || "未识别发言人"}${m.source?.includes("ocr") ? "（来自本机 OCR，请核对）" : ""}：${m.text}`;
    const next = byteLength(line) + 2;
    if (size + next > limit) break;
    size += next;
    lines.unshift(line);
  }
  return lines.join("\n\n");
}
export function setupChatReader(useContext?: (data: {title: string; platform: string; kind: "person" | "group"; context: string; incoming: string}) => void): void {
  const root = document.getElementById("chatReader");
  if (!root || root.dataset.initialized) return;
  root.dataset.initialized = "true";
  const el = <T extends HTMLElement>(id: string) =>
    document.getElementById(id) as T;
  const value = (id: string) => el<HTMLInputElement>(id).value.trim();
  const status = (text: string) => {
    el("crStatus").textContent = text;
  };
  let windows: WindowRow[] = [],
    bindings: Binding[] = [];
  let pending = "",
    seq = 0,
    running = false,
    timer = 0,
    generation = "",
    generationTimer = 0;
  let loaded: Session | undefined, generateFor: EventRow | undefined;
  let modelEvent: EventRow | undefined;
  const jobs = new Map<string, EventRow>();
  const revisions = new Map<string, number>();
  function send(
    action: Request["action"],
    extra: Partial<Request> = {},
  ): boolean {
    if (pending || !window.chrome?.webview) return false;
    pending = `chat-${Date.now()}-${++seq}`;
    window.chrome.webview.postMessage(
      serializeHostMessage({
        type: "chatRequest",
        data: { requestId: pending, action, ...extra },
      }),
    );
    window.clearTimeout(timer);
    timer = window.setTimeout(() => {
      pending = "";
      status("读取超时，已停止监测；记录保留。请重新扫描。");
      stop();
    }, 15000);
    return true;
  }
  function cancelModel() {
    window.clearTimeout(generationTimer);
    if (generation)
      window.chrome?.webview?.postMessage(
        serializeHostMessage({
          type: "assistantRequest",
          data: {
            requestId: `chat-cancel-${++seq}`,
            action: "cancel",
            origin: "chat",
          },
        }),
      );
    generation = "";
    modelEvent = undefined;
  }
  function stop() {
    running = false;
    jobs.clear();
    generateFor = undefined;
    cancelModel();
    el<HTMLInputElement>("crAuto").checked = false;
    el("crStart").textContent = "开始只读监测";
    window.chrome?.webview?.postMessage(
      serializeHostMessage({
        type: "chatRequest",
        data: { requestId: `chat-stop-${++seq}`, action: "cancel" },
      }),
    );
    pending = "";
    window.clearTimeout(timer);
    send("reset");
  }
  function bindingList() {
    const list = el("crBindings");
    list.replaceChildren();
    for (const b of bindings) {
      const row = document.createElement("div");
      row.className = "studio-toolbar";
      const name = document.createElement("span");
      name.textContent = `${platformNames[b.platform]} · ${b.account} / ${b.title}（${b.kind === "group" ? "群" : "人"}，${b.key}）`;
      const remove = document.createElement("button");
      remove.type = "button";
      remove.textContent = "解除本次绑定";
      remove.onclick = () => {
        bindings = bindings.filter((v) => v.key !== b.key);
        jobs.delete(b.key);
        revisions.delete(b.key);
        if (modelEvent?.key === b.key) cancelModel();
        bindingList();
      };
      row.append(name, remove);
      list.append(row);
    }
  }
  function showWindows(rows: WindowRow[]) {
    windows = rows;
    const select = el<HTMLSelectElement>("crWindows"),
      previous = select.value;
    select.replaceChildren(new Option("选择已识别的聊天窗口", ""));
    const reports: string[] = [];
    for (const [platform, name] of Object.entries(platformNames)) {
      const found = rows.filter((w) => w.platform === platform);
      reports.push(
        `${name}：${found.length ? found.map((w) => `${w.title || "未识别会话"} — ${w.status}`).join("；") : "未找到可见窗口"}`,
      );
    }
    rows.forEach((w) => {
      if (w.readable)
        select.add(
          new Option(
            `${platformNames[w.platform]} · ${w.title}（${w.messages.length} 条可读）`,
            w.window + "\n" + w.title,
          ),
        );
    });
    if ([...select.options].some((o) => o.value === previous))
      select.value = previous;
    el("crCapabilities").textContent = reports.join("\n");
  }
  function model(session: Session, event: EventRow) {
    if (
      !running ||
      !el<HTMLInputElement>("crAuto").checked ||
      revisions.get(event.key) !== event.revision ||
      !event.incoming.length
    )
      return;
    const incoming = chatContext(event.incoming, 11500);
    if (!incoming || !value("rsMode")) return;
    generation = `chat-generate-${Date.now()}-${++seq}`;
    modelEvent = event;
    // Keep the triggering batch out of historical context.
    const first = event.incoming[0];
    const index = session.messages.findIndex(
      (m) => m.id === first.id && m.text === first.text,
    );
    const context = chatContext(
      index < 0 ? session.messages : session.messages.slice(0, index),
    );
    window.chrome?.webview?.postMessage(
      serializeHostMessage({
        type: "assistantRequest",
        data: {
          action: "generate",
          origin: "chat",
          requestId: generation,
          mode: value("rsMode"),
          task: "reply",
          length: (value("rsLength") || "medium") as "short" | "medium" | "long",
          count: Number(value("rsCount")) as 3 | 5,
          incoming,
          context,
          intent: "为最新收到的消息准备回复；只输出草稿，不发送。",
        },
      }),
    );
    status(`正在为“${event.title}”准备回复；仍继续读取新消息。`);
    generationTimer = window.setTimeout(() => {
      cancelModel();
      status("模型请求超时，本次不自动重试；聊天记录已保存。");
    }, 50000);
  }
  function download(session: Session) {
    const blob = new Blob([JSON.stringify(session, null, 2)], {
      type: "application/json;charset=utf-8",
    });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `聊天记录-${session.key}.json`;
    a.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  el("crScan").onclick = () => {
    if (send("scan")) status("正在只读扫描聊天窗口…");
  };
  el("crUseContext").onclick = () => {
    if (!loaded || loaded.key !== value("crHistory")) {
      status("先选择并读取一份会话记录，再带入回复区。");
      return;
    }
    let last = loaded.messages.length - 1;
    while (last >= 0 && (loaded.messages[last].self || !loaded.messages[last].text.trim())) --last;
    if (last < 0) {
      status("该会话没有可回复的对方文字消息。");
      return;
    }
    useContext?.({ title: loaded.title, platform: platformNames[loaded.platform] || loaded.platform,
      kind: loaded.kind === "group" ? "group" : "person", context: chatContext(loaded.messages.slice(0, last)),
      incoming: chatContext([loaded.messages[last]], 11500) });
  };
  el("crWindows").onchange = () => {
    const w = windows.find(
      (w) => w.window + "\n" + w.title === value("crWindows"),
    );
    if (!w || !value("crWindows")) return;
    el<HTMLSelectElement>("crKind").value =
      w.kind === "group" ? "group" : "person";
    el("crPreview").textContent =
      w.messages
        .slice(-5)
        .map((m) => `${m.speaker || "未识别发言人"}：${m.text}`)
        .join("\n\n") || "暂无可读文字";
  };
  el("crBind").onclick = () => {
    const w = value("crWindows")
      ? windows.find((w) => w.window + "\n" + w.title === value("crWindows"))
      : undefined;
    const key = value("crKey"),
      account = value("crAccount"),
      self = value("crSelf");
    if (!w || !w.readable || !/^[\w-]{1,64}$/.test(key) || !account || !self) {
      status("请选择可读聊天，填写账户标识、自己的昵称和独立会话编号。");
      return;
    }
    if (
      bindings.some(
        (b) => b.key === key || (b.window === w.window && b.title === w.title),
      )
    ) {
      status("此窗口或编号已经绑定；请先解除原绑定。");
      return;
    }
    if (bindings.length >= 20) {
      status("最多同时绑定 20 个会话。");
      return;
    }
    bindings.push({
      key,
      account,
      self,
      window: w.window,
      title: w.title,
      platform: w.platform,
      kind: value("crKind"),
    });
    bindingList();
    status("已绑定。切回此聊天时继续记录；首次读取只建立基线，不触发回复。");
  };
  el("crStart").onclick = () => {
    if (running) {
      stop();
      status("已停止读取和自动预回复，记录保留。");
      return;
    }
    if (!bindings.length) {
      status("请先扫描并绑定要记录的聊天。");
      return;
    }
    running = true;
    el("crStart").textContent = "停止监测";
    status(
      "只读监测已开始。关闭设置窗口会停止；不会点击、切换或发送聊天消息。",
    );
  };
  el("crAuto").onchange = () => {
    jobs.clear();
    cancelModel();
    status(
      el<HTMLInputElement>("crAuto").checked
        ? "后续确认的新消息将连同该会话近期上下文发送到已配置的模型，自动生成多条草稿。"
        : "自动预回复已关闭；仍可只读记录。",
    );
  };
  for (const id of ["rsMode", "rsCount"])
    el(id)?.addEventListener("change", () => {
      jobs.clear();
      cancelModel();
      el("crReplies").replaceChildren();
    });
  el("crHistoryRefresh").onclick = () => send("overview");
  el("crHistory").onchange = () => {
    if (value("crHistory")) {
      generateFor = undefined;
      send("load", { key: value("crHistory") });
    }
  };
  el("crExport").onclick = () => {
    if (loaded && loaded.key === value("crHistory")) download(loaded);
    else status("请先读取一个保存的会话。");
  };
  el("crDelete").onclick = () => {
    if (pending) {
      status("请等当前读取结束后再删除。");
      return;
    }
    const key = value("crHistory");
    if (
      key &&
      window.confirm("删除这个会话的本地聊天记录？聊天软件中的记录不受影响。")
    ) {
      bindings = bindings.filter((b) => b.key !== key);
      bindingList();
      jobs.delete(key);
      revisions.delete(key);
      if (modelEvent?.key === key) cancelModel();
      generateFor = undefined;
      if (send("delete", { key })) {
        loaded = undefined;
        el("crHistoryText").textContent = "";
      }
    }
  };
  onHostMessage("chatResponse", (message) => {
    if (message.requestId !== pending) return;
    pending = "";
    window.clearTimeout(timer);
    if (!message.ok) {
      stop();
      status(message.message);
      return;
    }
    if (message.windows) {
      showWindows(message.windows);
      if (message.action === "scan")
        status(
          `扫描完成：${message.windows.filter((w) => w.readable).length} 个可读会话，请核对预览并绑定。`,
        );
      for (const binding of bindings)
        if (
          !message.windows.some(
            (w) =>
              w.window === binding.window &&
              w.title === binding.title &&
              w.readable,
          )
        ) {
          jobs.delete(binding.key);
          revisions.delete(binding.key);
          if (modelEvent?.key === binding.key) cancelModel();
        }
    }
    if (message.sessions) {
      const select = el<HTMLSelectElement>("crHistory"),
        previous = select.value;
      select.replaceChildren(new Option("选择已记录会话", ""));
      for (const s of message.sessions)
        select.add(
          new Option(
            `${platformNames[s.platform]} · ${s.account} / ${s.title} · ${s.count} 条 [${s.key}]`,
            s.key,
          ),
        );
      select.value = previous;
    }
    for (const event of message.events ?? []) {
      if (!bindings.some((b) => b.key === event.key)) continue;
      revisions.set(event.key, event.revision);
      if (modelEvent?.key === event.key) cancelModel();
      for (const card of el("crReplies").querySelectorAll<HTMLElement>(
        "[data-key]",
      ))
        if (card.dataset.key === event.key) {
          card.dataset.stale = "true";
          card.classList.add("studio-stale");
          const note = card.querySelector("small");
          if (note)
            note.textContent = "已有更新消息，这组草稿仅针对之前的消息。";
        }
      if (
        running &&
        el<HTMLInputElement>("crAuto").checked &&
        event.incoming.length
      ) {
        if (jobs.size >= 5 && !jobs.has(event.key)) {
          status("待回复会话超过 5 个，已跳过额外自动请求；原文仍记录。");
          continue;
        }
        jobs.set(event.key, event);
      } else jobs.delete(event.key);
    }
    if (message.session) {
      if (generateFor?.key === message.session.key) {
        const event = generateFor;
        generateFor = undefined;
        model(message.session, event);
      } else {
        loaded = message.session;
        el("crHistoryText").textContent =
          chatContext(loaded.messages, 100000) || "暂无文字记录";
        status(
          `已读取 ${loaded.messages.length} 条记录，预览最多 100 KB；导出包含全部记录。`,
        );
      }
    }
  });
  onHostMessage("assistantResponse", (message) => {
    if (message.requestId !== generation) return;
    const event = modelEvent;
    generation = "";
    modelEvent = undefined;
    window.clearTimeout(generationTimer);
    if (!event || !running || revisions.get(event.key) !== event.revision)
      return;
    if (!message.ok) {
      status(message.message);
      return;
    }
    const group = document.createElement("article");
    group.className = "studio-reply";
    group.dataset.key = event.key;
    const heading = document.createElement("h3");
    heading.textContent = `${event.title} · ${new Date().toLocaleTimeString()} · 回复草稿`;
    const source = document.createElement("p");
    source.textContent = `针对：${chatContext(event.incoming, 3000)}`;
    const note = document.createElement("small");
    note.textContent = "仅为备选，尚未发送。";
    group.append(heading, source, note);
    for (const row of message.replies ?? []) {
      const p = document.createElement("p");
      p.textContent = `${row.label}：${row.text}`;
      const copy = document.createElement("button");
      copy.textContent = "复制这条";
      copy.type = "button";
      copy.onclick = () =>
        window.chrome?.webview?.postMessage(
          serializeHostMessage({ type: "copyText", data: row.text }),
        );
      group.append(p, copy);
    }
    const replies = el("crReplies");
    replies.prepend(group);
    while (replies.children.length > 10) replies.lastElementChild?.remove();
    status(`“${event.title}”的备选已准备好，请自行选择和发送。`);
  });
  window.setInterval(() => {
    if (pending) return;
    if (running && !generation && jobs.size) {
      const event = jobs.values().next().value as EventRow;
      jobs.delete(event.key);
      generateFor = event;
      if (!send("load", { key: event.key })) generateFor = undefined;
      return;
    }
    if (running) send("poll", { bindings });
  }, 3000);
  window.addEventListener("msime-chat-hidden", () => {
    if (running || generation) stop();
  });
  window.addEventListener("beforeunload", () => {
    running = false;
    cancelModel();
  });
  send("overview");
}
