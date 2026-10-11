import { setupChatReader } from "./chat-reader";
import { quickModes, scenarios, taskDefaultMode, type WritingTask } from "./reply-workflow";
import { onHostMessage } from "../utils/host-messages";
import {
  serializeHostMessage,
  type ClientMessage,
} from "../../../../shared/messages";
import "../styles/modules/reply-studio.css";
type Request = Extract<ClientMessage, { type: "assistantRequest" }>["data"];
type Action = Request["action"];
const bytes = (text: string) => new TextEncoder().encode(text).length;
export function contextWithDraft(
  context: string,
  incoming: string,
  draft: string,
): string {
  return [
    context.trim(),
    `对方 / 原文：${incoming.trim()}\n我的回复草稿（尚未发送）：${draft.trim()}`,
  ]
    .filter(Boolean)
    .join("\n\n");
}
export function setupReplyStudio(): void {
  const root = document.getElementById("replyStudio");
  if (!root || root.dataset.initialized) return;
  root.dataset.initialized = "true";
  const el = <T extends HTMLElement>(id: string) =>
    document.getElementById(id) as T;
  const value = (id: string) => el<HTMLInputElement>(id)?.value ?? "";
  const status = (s: string) => {
    el("rsStatus").textContent = s;
  };
  let draftIncoming = "",
    sessionId = "",
    waiting = "",
    waitingAction: Action = "overview",
    counter = 0,
    revision = 0,
    requestRevision = 0,
    dirty = false;
  let timer: number | undefined, debounce: number | undefined;
  let descriptions = new Map<string, string>();
  let modeNames = new Map<string, string>();
  const task = () => (value("rsTask") || "reply") as WritingTask;
  function renderWorkflow() {
    el("rsIncomingLabel").textContent = task() === "reply" ? "对方刚发来的消息" : task() === "rewrite" ? "我想改写的话" : "真实素材 / 要写的事情";
    el("rsGenerate").textContent = `生成 ${value("rsCount") || "3"} 条${task() === "reply" ? "回复" : "表达"}`;
    el("rsQuickModes").hidden = task() !== "reply";
    const list = el("rsQuickModes");
    list.replaceChildren();
    for (const id of quickModes) {
      if (!modeNames.has(id)) continue;
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = modeNames.get(id)!;
      button.className = value("rsMode") === id ? "studio-primary" : "";
      button.onclick = () => {
        el<HTMLSelectElement>("rsMode").value = id;
        changed({ target: el("rsMode") } as unknown as Event);
        el("rsModeDescription").textContent = descriptions.get(id) ?? "";
        renderWorkflow();
      };
      list.append(button);
    }
  }
  function preview() {
    el("rsContextSize").textContent = `（${value("rsContext").length} 字）`;
    el("rsSendPreview").textContent =
      `将发送：本条消息 ${value("rsIncoming").length} 字、要求 ${value("rsIntent").length} 字${el<HTMLInputElement>("rsUseContext").checked ? `、本会话背景 ${value("rsContext").length} 字、摘要 ${value("rsSummary").length} 字` : "；不使用历史上下文"}。`;
    el("rsActiveContext").textContent = el<HTMLInputElement>("rsUseContext").checked
      ? `当前对象：${value("rsTitle") || "临时会话"} · ${value("rsKind") === "group" ? "群聊" : "联系人"} · 背景 ${value("rsContext").length} 字 / 摘要 ${value("rsSummary").length} 字`
      : "本次不使用历史上下文";
  }
  function busy(on: boolean) {
    for (const id of [
      "rsGenerate",
      "rsRegenerate",
      "rsSummarize",
      "rsSave",
      "rsReload",
      "rsDelete",
      "rsRemember",
    ])
      el<HTMLButtonElement>(id).disabled = on;
  }
  function cancel() {
    window.clearTimeout(timer);
    window.clearTimeout(debounce);
    if (waiting) {
      const id = `reply-cancel-${++counter}`;
      window.chrome?.webview?.postMessage(
        serializeHostMessage({
          type: "assistantRequest",
          data: { requestId: id, action: "cancel" },
        }),
      );
    }
    waiting = "";
    busy(false);
  }
  function request(action: Action, extra: Partial<Request> = {}) {
    if (waiting) {
      status("请等待当前操作结束，或先取消。");
      return;
    }
    if (!window.chrome?.webview) {
      status("请在水杉原生设置窗口中使用；浏览器预览不会调用模型。");
      return;
    }
    const context = value("rsContext"),
      incoming = value("rsIncoming"),
      summary = value("rsSummary");
    if (
      bytes(context) > 48000 ||
      bytes(incoming) > 12000 ||
      bytes(summary) > 12000 ||
      bytes(value("rsIntent")) > 4000
    ) {
      status("内容过长，请分段导入或精简后再试。");
      return;
    }
    if (action === "generate" && (!incoming.trim() || !value("rsMode"))) {
      status("先填写消息或原文，并选择表达模式。");
      return;
    }
    if (action === "summarize" && !context.trim() && !incoming.trim()) {
      status("先添加需要总结的内容。");
      return;
    }
    const id = `reply-${Date.now()}-${++counter}`;
    const data: Request = { requestId: id, action, ...extra };
    if (action === "generate" || action === "summarize")
      Object.assign(data, {
        context:
          action === "summarize" || el<HTMLInputElement>("rsUseContext").checked
            ? context
            : "",
        summary: el<HTMLInputElement>("rsUseContext").checked ? summary : "",
        incoming,
        intent: value("rsIntent"),
        mode: value("rsMode"),
        task: task(),
        length: (value("rsLength") || "medium") as "short" | "medium" | "long",
        relationship: value("rsRelationship"),
        count: Number(value("rsCount")) as 3 | 5,
      });
    if (action === "save") {
      sessionId ||= crypto.randomUUID();
      Object.assign(data, {
        sessionId,
        title: value("rsTitle").trim() || "未命名会话",
        context,
        summary,
        kind: (value("rsKind") || "person") as "person" | "group",
        platform: value("rsPlatform").trim(),
        retentionDays: Number(value("rsRetention")) as 30 | 90 | 365,
      });
    }
    waiting = id;
    waitingAction = action;
    requestRevision = revision;
    busy(true);
    status(
      action === "generate"
        ? "正在准备不同表达…"
        : action === "summarize"
          ? "正在总结，原文会保留…"
          : "正在处理本机会话…",
    );
    window.chrome.webview.postMessage(
      serializeHostMessage({ type: "assistantRequest", data }),
    );
    timer = window.setTimeout(() => {
      cancel();
      status("请求超时，原文和草稿都已保留。");
    }, 50000);
  }
  function fresh() {
    cancel();
    el("rsDeleteConfirm").hidden = true;
    revision++;
    sessionId = "";
    draftIncoming = "";
    dirty = false;
    for (const id of [
      "rsTitle",
      "rsContext",
      "rsSummary",
      "rsIncoming",
      "rsIntent",
      "rsRelationship",
      "rsDraft",
      "rsPlatform",
      "rsSpeaker",
      "rsMessageTime",
      "rsMessageText",
    ])
      el<HTMLInputElement>(id).value = "";
    el<HTMLSelectElement>("rsSessions").value = "";
    el<HTMLSelectElement>("rsKind").value = "person";
    el("rsReplies").replaceChildren();
    el<HTMLInputElement>("rsAuto").checked = false;
    preview();
    status("新的临时会话，不读取其他会话。");
  }
  function confirmDiscard(): boolean {
    return (
      (!dirty &&
        !value("rsIncoming").trim() &&
        !value("rsDraft").trim() &&
        !value("rsMessageText").trim()) ||
      window.confirm("当前修改还没有保存，确定切换并丢弃这些修改吗？")
    );
  }
  function copy(text: string) {
    if (!text.trim()) {
      status("草稿为空。");
      return;
    }
    if (window.chrome?.webview) {
      window.chrome.webview.postMessage(
        serializeHostMessage({ type: "copyText", data: text }),
      );
      status("已请求复制，可粘贴后自行发送。");
    } else
      navigator.clipboard
        .writeText(text)
        .then(() => status("已复制。"))
        .catch(() => status("复制失败，请手动选择文字复制。"));
  }
  el("rsVoiceSettings").onclick = () => {
    (
      document.querySelector(
        '.sidebar .item[data-target="voice"]',
      ) as HTMLElement | null
    )?.click();
  };
  el("rsGenerate").onclick = () => request("generate");
  el("rsRegenerate").onclick = () => request("generate");
  el("rsExample").onclick = () => {
    if ((value("rsIncoming").trim() || value("rsIntent").trim()) && !window.confirm("用示例替换当前原文和要求？已保存的上下文不受影响。")) return;
    const example = scenarios[value("rsMode")] ?? (task() === "rewrite"
      ? { incoming: "你这个方案还没写完，明天就要交了，能不能快一点。", intent: "保留催进度的意思，说得明确又礼貌", relationship: "同事" }
      : scenarios.high_eq);
    el<HTMLInputElement>("rsIncoming").value = example.incoming;
    el<HTMLInputElement>("rsIntent").value = example.intent;
    el<HTMLInputElement>("rsRelationship").value = example.relationship;
    changed({ target: el("rsIntent") } as unknown as Event);
    status("已填入示例，尚未请求模型；可修改后生成。");
  };
  el("rsTask").onchange = () => {
    const next = taskDefaultMode(task());
    if (modeNames.has(next)) el<HTMLSelectElement>("rsMode").value = next;
    changed({ target: el("rsTask") } as unknown as Event);
    el("rsModeDescription").textContent = descriptions.get(value("rsMode")) ?? "";
    renderWorkflow();
  };
  el("rsSummarize").onclick = () => request("summarize");
  el("rsSave").onclick = () => request("save");
  el("rsReload").onclick = () => request("overview");
  el("rsCancel").onclick = () => {
    cancel();
    status("已取消，原文与草稿保留。");
  };
  el("rsNew").onclick = () => {
    if (confirmDiscard()) fresh();
  };
  el("rsSessions").onchange = () => {
    const id = value("rsSessions");
    if (!confirmDiscard()) {
      el<HTMLSelectElement>("rsSessions").value = sessionId;
      return;
    }
    fresh();
    if (id) request("load", { sessionId: id });
  };
  el("rsDelete").onclick = () => {
    if (!sessionId) {
      status("此临时会话尚未保存。");
      return;
    }
    el("rsDeleteConfirm").hidden = false;
  };
  el("rsDeleteNo").onclick = () => {
    el("rsDeleteConfirm").hidden = true;
  };
  el("rsDeleteYes").onclick = () => {
    el("rsDeleteConfirm").hidden = true;
    request("delete", { sessionId });
  };
  el("rsAppend").onclick = () => {
    const speaker = value("rsSpeaker").trim(),
      text = value("rsMessageText").trim();
    if (!speaker || !text) {
      status("请填写发言人和发言内容。");
      return;
    }
    const time = value("rsMessageTime") || "时间未提供";
    const next = [value("rsContext").trim(), `[${time}] ${speaker}：\n${text}`]
      .filter(Boolean)
      .join("\n\n");
    if (bytes(next) > 48000) {
      status("本会话记录已满，请先导出备份并精简。");
      return;
    }
    cancel();
    el<HTMLTextAreaElement>("rsContext").value = next;
    el<HTMLTextAreaElement>("rsMessageText").value = "";
    dirty = true;
    revision++;
    preview();
    status("已追加到当前记录，点击“保存上下文”后加密保存。");
  };
  el("rsCopyDraft").onclick = () => copy(value("rsDraft"));
  document.getElementById("rsVoiceDraft")?.addEventListener("click", async () => {
    const {openVoiceStudio} = await import('./voice-studio');
    await openVoiceStudio(value("rsDraft"));
  });
  el("rsRemember").onclick = () => {
    if (!value("rsDraft").trim()) {
      status("先选取或填写一条草稿。");
      return;
    }
    if (draftIncoming !== value("rsIncoming")) {
      status("草稿来自上一条消息，请重新选择或编辑草稿后再加入记录。");
      return;
    }
    const next = contextWithDraft(
      value("rsContext"),
      value("rsIncoming"),
      value("rsDraft"),
    );
    if (bytes(next) > 48000) {
      status("上下文已满，请先精简或新建会话。");
      return;
    }
    el<HTMLTextAreaElement>("rsContext").value = next;
    dirty = true;
    revision++;
    preview();
    request("save");
  };
  el("rsExport").onclick = () => {
    const text = `${value("rsTitle")}\n类型：${value("rsKind") === "group" ? "群聊" : "联系人"}\n来源：${value("rsPlatform")}\n\n上下文：\n${value("rsContext")}\n\n摘要：\n${value("rsSummary")}`;
    const url = URL.createObjectURL(
      new Blob(["\uFEFF" + text], { type: "text/plain;charset=utf-8" }),
    );
    const a = document.createElement("a");
    a.href = url;
    a.download = "回复会话.txt";
    a.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  el("rsImport").onchange = async () => {
    const file = el<HTMLInputElement>("rsImport").files?.[0];
    if (!file) return;
    if (file.size > 48000) {
      status("文件不能超过 48 KB。");
      return;
    }
    const importRevision = revision;
    try {
      const text = new TextDecoder("utf-8", { fatal: true }).decode(
        await file.arrayBuffer(),
      );
      if (revision !== importRevision) {
        status("会话已发生变化，请重新导入。");
        return;
      }
      if (dirty && !window.confirm("导入会替换当前上下文，是否继续？")) return;
      cancel();
      el<HTMLTextAreaElement>("rsContext").value = text;
      dirty = true;
      revision++;
      preview();
      status("已导入到草稿，尚未保存或发送。");
    } catch {
      status("无法读取，请选择 UTF-8 文本文件。");
    }
  };
  function changed(event: Event) {
    if (event.target === el("rsDraft")) {
      draftIncoming = value("rsIncoming");
      dirty = true;
      return; // Editing a chosen draft must not erase the other suggestions.
    }
    dirty = true;
    revision++;
    if (waitingAction === "generate" || waitingAction === "summarize") cancel();
    el("rsReplies").replaceChildren();
    preview();
    if (
      event.target === el("rsIncoming") &&
      el<HTMLInputElement>("rsAuto").checked
    ) {
      window.clearTimeout(debounce);
      debounce = window.setTimeout(() => {
        if (
          root?.getClientRects().length &&
          !document.hidden &&
          value("rsIncoming").trim()
        )
          request("generate");
      }, 1200);
    }
  }
  for (const id of [
    "rsContext",
    "rsSummary",
    "rsIncoming",
    "rsIntent",
    "rsRelationship",
    "rsTitle",
    "rsDraft",
    "rsPlatform",
    "rsSpeaker",
    "rsMessageTime",
    "rsMessageText",
  ])
    el(id).addEventListener("input", changed);
  for (const id of [
    "rsMode",
    "rsCount",
    "rsUseContext",
    "rsRetention",
    "rsKind",
    "rsLength",
  ])
    el(id).addEventListener("change", (e) => {
      changed(e);
      el("rsModeDescription").textContent =
        descriptions.get(value("rsMode")) ?? "";
      renderWorkflow();
    });
  el("rsAuto").onchange = () => {
    window.clearTimeout(debounce);
    status(
      el<HTMLInputElement>("rsAuto").checked
        ? "已开启：以后在此框粘贴或修改消息，停顿后会联网准备回复。"
        : "自动预备已关闭。",
    );
  };
  onHostMessage("assistantResponse", (message) => {
    if (message.requestId !== waiting) return;
    window.clearTimeout(timer);
    waiting = "";
    busy(false);
    if (!message.ok) {
      status(message.message);
      return;
    }
    if (message.modes) {
      const mode = el<HTMLSelectElement>("rsMode"),
        previous = mode.value;
      mode.replaceChildren();
      descriptions = new Map(message.modes.map((m) => [m.id, m.description]));
      modeNames = new Map(message.modes.map((m) => [m.id, m.name]));
      for (const m of message.modes) mode.add(new Option(m.name, m.id));
      if (descriptions.has(previous)) mode.value = previous;
      else if (modeNames.has(taskDefaultMode(task()))) mode.value = taskDefaultMode(task());
      el("rsModeDescription").textContent = descriptions.get(mode.value) ?? "";
      renderWorkflow();
    }
    if (message.sessions) {
      const select = el<HTMLSelectElement>("rsSessions");
      select.replaceChildren(new Option("未保存的临时会话", ""));
      for (const row of message.sessions)
        select.add(
          new Option(
            `${row.kind === "group" ? "群" : "人"} · ${row.platform ? row.platform + " / " : ""}${row.title}`,
            row.sessionId,
          ),
        );
      select.value = sessionId;
    }
    if (message.action === "delete") {
      fresh();
      status("本机会话已删除。");
      return;
    }
    if (message.session && requestRevision === revision) {
      const s = message.session;
      sessionId = s.sessionId;
      el<HTMLSelectElement>("rsKind").value = s.kind ?? "person";
      el<HTMLInputElement>("rsPlatform").value = s.platform ?? "";
      el<HTMLInputElement>("rsTitle").value = s.title;
      el<HTMLTextAreaElement>("rsContext").value = s.context;
      el<HTMLTextAreaElement>("rsSummary").value = s.summary;
      el<HTMLSelectElement>("rsRetention").value = String(s.retentionDays);
      el<HTMLSelectElement>("rsSessions").value = sessionId;
      dirty = false;
      preview();
    }
    if (requestRevision !== revision) {
      status("操作已完成，保留了你随后修改的草稿。");
      return;
    }
    if (message.summary) {
      el<HTMLTextAreaElement>("rsSummary").value = message.summary;
      dirty = true;
      preview();
    }
    if (message.replies) {
      const list = el("rsReplies");
      list.replaceChildren();
      message.replies.forEach((row, i) => {
        const card = document.createElement("article");
        card.className = "studio-reply";
        const title = document.createElement("h3");
        title.textContent = `${i + 1}. ${row.label}`;
        const p = document.createElement("p");
        p.textContent = row.text;
        const buttons = document.createElement("div");
        buttons.className = "studio-toolbar";
        const choose = document.createElement("button");
        choose.textContent = "选为草稿";
        choose.onclick = () => {
          if (value("rsDraft").trim() && value("rsDraft") !== row.text && !window.confirm("替换当前已编辑的草稿？")) return;
          el<HTMLTextAreaElement>("rsDraft").value = row.text;
          draftIncoming = value("rsIncoming");
          dirty = true;
          status("已选为草稿，可修改后复制；尚未发送。");
        };
        const copyButton = document.createElement("button");
        copyButton.textContent = "复制";
        copyButton.onclick = () => copy(row.text);
        const voiceButton = document.createElement("button");
        voiceButton.textContent = "转成语音";
        voiceButton.onclick = async () => { const {openVoiceStudio} = await import('./voice-studio'); await openVoiceStudio(row.text); };
        buttons.append(choose, copyButton, voiceButton);
        card.append(title, p, buttons);
        list.append(card);
      });
      list.scrollIntoView?.({ block: "start", behavior: "smooth" });
    }
    status(
      message.action === "save"
        ? "上下文已加密保存。"
        : message.action === "summarize"
          ? "摘要已生成，原文保留。确认后可保存。"
          : message.action === "generate"
            ? "备选已准备好，请选择或修改。"
            : "本机会话和模式已就绪。",
    );
  });
  setupChatReader((session) => {
    if (!confirmDiscard()) return;
    fresh();
    el<HTMLInputElement>("rsTitle").value = session.title;
    el<HTMLInputElement>("rsPlatform").value = session.platform;
    el<HTMLSelectElement>("rsKind").value = session.kind;
    el<HTMLTextAreaElement>("rsContext").value = session.context;
    el<HTMLTextAreaElement>("rsIncoming").value = session.incoming;
    el<HTMLSelectElement>("rsTask").value = "reply";
    el<HTMLInputElement>("rsUseContext").checked = true;
    dirty = true;
    preview();
    renderWorkflow();
    el("rsIncoming").scrollIntoView({ block: "center", behavior: "smooth" });
    status("已带入这个会话的记录和最后一条对方消息，请核对后生成；尚未联网。");
  });
  renderWorkflow();
  preview();
  request("overview");
}
