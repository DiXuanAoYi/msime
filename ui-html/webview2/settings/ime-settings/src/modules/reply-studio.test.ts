import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { readFileSync } from "node:fs";
vi.mock("./chat-reader", () => ({ setupChatReader: vi.fn() }));
const handlers = vi.hoisted(() => new Set<(m: any) => void>());
vi.mock("../utils/host-messages", () => ({
  onHostMessage: (_t: string, h: (m: any) => void) => handlers.add(h),
}));
class Element {
  value = "";
  textContent = "";
  disabled = false;
  checked = false;
  hidden = false;
  dataset: any = {};
  children: Element[] = [];
  files: any[] = [];
  onclick: any;
  onchange: any;
  events = new Map<string, any>();
  addEventListener(n: string, fn: any) {
    this.events.set(n, fn);
  }
  emit(n: string) {
    return (
      n === "click"
        ? this.onclick
        : n === "change" && this.onchange
          ? this.onchange
          : this.events.get(n)
    )?.({ target: this });
  }
  replaceChildren(...c: Element[]) {
    this.children = c;
  }
  append(...c: Element[]) {
    this.children.push(...c);
  }
  add(o: Element) {
    this.children.push(o);
    if (!this.value) this.value = o.value;
  }
  getClientRects() {
    return [1];
  }
}
let els: Map<string, Element>, sent: any[];
const get = (id: string) => els.get(id)!;
const last = () => sent.at(-1).data;
function reply(data: any = {}) {
  const r = last();
  for (const h of handlers)
    h({
      requestId: r.requestId,
      action: r.action,
      ok: true,
      message: "ok",
      ...data,
    });
}
function input(id: string, value: string) {
  get(id).value = value;
  get(id).emit("input");
}
beforeEach(async () => {
  vi.resetModules();
  vi.useFakeTimers();
  handlers.clear();
  sent = [];
  const html = readFileSync(
    new URL("../partials/reply-studio.html", import.meta.url),
    "utf8",
  );
  els = new Map(
    [...html.matchAll(/id="([^"]+)"/g)].map((m) => [m[1], new Element()]),
  );
  get("rsCount").value = "3";
  get("rsRetention").value = "90";
  get("rsUseContext").checked = true;
  vi.stubGlobal("document", {
    getElementById: (id: string) => get(id),
    createElement: () => new Element(),
    hidden: false,
  });
  vi.stubGlobal("window", {
    chrome: {
      webview: { postMessage: (s: string) => sent.push(JSON.parse(s)) },
    },
    setTimeout,
    clearTimeout,
    confirm: () => true,
  });
  vi.stubGlobal(
    "Option",
    class extends Element {
      constructor(text: string, value: string) {
        super();
        this.textContent = text;
        this.value = value;
      }
    },
  );
  const mod = await import("./reply-studio");
  mod.setupReplyStudio();
  reply({
    modes: [{ id: "high_eq", name: "高情商", description: "有温度" }],
    sessions: [],
  });
});
afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});
it("reads local catalog without sending content or generating automatically", () => {
  expect(sent).toHaveLength(1);
  expect(last().action).toBe("overview");
  input("rsIncoming", "今天有空吗");
  vi.advanceTimersByTime(3000);
  expect(sent).toHaveLength(1);
});
it("sends the selected count and respects history opt-out", () => {
  input("rsContext", "private history");
  input("rsSummary", "private summary");
  input("rsIncoming", "hello");
  get("rsUseContext").checked = false;
  get("rsCount").value = "5";
  get("rsGenerate").emit("click");
  expect(last()).toMatchObject({
    action: "generate",
    context: "",
    summary: "",
    incoming: "hello",
    count: 5,
    mode: "high_eq",
  });
});
it("drops replies after edits and after switching to a new conversation", () => {
  input("rsIncoming", "old");
  get("rsGenerate").emit("click");
  const old = last();
  input("rsIncoming", "new");
  for (const h of handlers)
    h({
      requestId: old.requestId,
      ok: true,
      replies: [{ label: "late", text: "old response" }],
    });
  expect(get("rsReplies").children).toHaveLength(0);
  expect(get("rsIncoming").value).toBe("new");
  get("rsNew").emit("click");
  expect(get("rsContext").value).toBe("");
  expect(get("rsIncoming").value).toBe("");
  expect(get("rsAuto").checked).toBe(false);
});
it("preserves original context when summarizing and renders model text literally", () => {
  input("rsContext", "original conversation");
  get("rsSummarize").emit("click");
  reply({ summary: "facts" });
  expect(get("rsContext").value).toBe("original conversation");
  expect(get("rsSummary").value).toBe("facts");
  input("rsIncoming", "hello");
  get("rsGenerate").emit("click");
  reply({
    replies: [{ label: "<b>bold</b>", text: "<img src=x onerror=alert(1)>" }],
  });
  const card = get("rsReplies").children[0];
  expect(card.children[1].textContent).toBe("<img src=x onerror=alert(1)>");
  card.children[2].children[0].onclick();
  expect(get("rsDraft").value).toContain("<img");
});
it("keeps input and draft on timeout and rejects late responses", () => {
  input("rsIncoming", "keep incoming");
  input("rsDraft", "keep draft");
  get("rsGenerate").emit("click");
  const old = last();
  vi.advanceTimersByTime(50001);
  expect(get("rsStatus").textContent).toContain("超时");
  expect(get("rsDraft").value).toBe("keep draft");
  expect(get("rsIncoming").value).toBe("keep incoming");
  for (const h of handlers)
    h({ requestId: old.requestId, ok: true, summary: "late" });
  expect(get("rsSummary").value).toBe("");
});
it("debounces only explicitly enabled incoming-message pre-generation", () => {
  get("rsAuto").checked = true;
  input("rsIncoming", "first");
  vi.advanceTimersByTime(800);
  input("rsIncoming", "latest");
  vi.advanceTimersByTime(1201);
  expect(last()).toMatchObject({ action: "generate", incoming: "latest" });
  expect(sent.filter((x) => x.data?.action === "generate")).toHaveLength(1);
});
it("does not overwrite edits made while a saved context is loading", () => {
  get("rsSessions").value = "alice";
  get("rsSessions").emit("change");
  input("rsContext", "new local edit");
  reply({
    session: {
      sessionId: "alice",
      title: "A",
      context: "old",
      summary: "",
      retentionDays: 90,
    },
  });
  expect(get("rsContext").value).toBe("new local edit");
});
it("rejects oversized UTF-8 text before sending and marks remembered text as unsent", async () => {
  input("rsIncoming", "🙂".repeat(3001));
  get("rsGenerate").emit("click");
  expect(sent).toHaveLength(1);
  expect(get("rsStatus").textContent).toContain("过长");
  const { contextWithDraft } = await import("./reply-studio");
  expect(contextWithDraft("history", "question", "answer")).toBe(
    "history\n\n对方 / 原文：question\n我的回复草稿（尚未发送）：answer",
  );
});
it("does not apply imported text after another edit or session switch", async () => {
  let finish!: (buffer: ArrayBuffer) => void;
  get("rsImport").files = [
    {
      size: 5,
      arrayBuffer: () =>
        new Promise<ArrayBuffer>((r) => {
          finish = r;
        }),
    },
  ];
  const task = get("rsImport").emit("change");
  input("rsContext", "newer");
  finish(new TextEncoder().encode("older").buffer);
  await task;
  expect(get("rsContext").value).toBe("newer");
});

it("saves separate group identity and appends attributed original speech", () => {
  get("rsKind").value = "group";
  input("rsPlatform", "微信");
  input("rsTitle", "项目群");
  input("rsSpeaker", "小林");
  input("rsMessageTime", "2026-10-10T12:00");
  input("rsMessageText", "周日下午聚餐");
  get("rsAppend").emit("click");
  expect(get("rsContext").value).toBe(
    "[2026-10-10T12:00] 小林：\n周日下午聚餐",
  );
  expect(get("rsMessageText").value).toBe("");
  get("rsSave").emit("click");
  expect(last()).toMatchObject({
    action: "save",
    kind: "group",
    platform: "微信",
    title: "项目群",
    context: "[2026-10-10T12:00] 小林：\n周日下午聚餐",
  });
});
it("does not attach an old draft to a newly received message", () => {
  input("rsIncoming", "old question");
  input("rsDraft", "old answer");
  input("rsIncoming", "new question");
  get("rsRemember").emit("click");
  expect(sent).toHaveLength(1);
  expect(get("rsStatus").textContent).toContain("上一条消息");
  expect(get("rsDraft").value).toBe("old answer");
});

it("still confirms unsaved reply loss after successfully saving only context", () => {
  input("rsIncoming", "unsaved message");
  input("rsDraft", "unsaved reply");
  get("rsSave").emit("click");
  reply({
    session: {
      sessionId: last().sessionId,
      title: "A",
      context: "",
      summary: "",
      retentionDays: 90,
    },
  });
  const confirm = vi.fn(() => false);
  window.confirm = confirm;
  get("rsNew").emit("click");
  expect(confirm).toHaveBeenCalledOnce();
  expect(get("rsDraft").value).toBe("unsaved reply");
});

it("keeps alternative replies when editing the selected draft", () => {
  input("rsIncoming", "明天聊聊？");
  get("rsGenerate").emit("click");
  reply({replies: [{label: "轻松", text: "好呀"}, {label: "明确", text: "可以，下午如何"}]});
  const card = get("rsReplies").children[0];
  card.children[2].children[0].emit("click");
  input("rsDraft", "好呀，明天下午见");
  expect(get("rsReplies").children).toHaveLength(2);
  expect(get("rsDraft").value).toBe("好呀，明天下午见");
});
it("sends writing task, length and relationship without turning on auto generation", () => {
  input("rsIncoming", "这句话需要润色");
  get("rsTask").value = "rewrite";
  get("rsTask").emit("change");
  get("rsLength").value = "short";
  input("rsRelationship", "同事");
  get("rsGenerate").emit("click");
  expect(last()).toMatchObject({task: "rewrite", length: "short", relationship: "同事"});
  expect(get("rsAuto").checked).toBe(false);
});
