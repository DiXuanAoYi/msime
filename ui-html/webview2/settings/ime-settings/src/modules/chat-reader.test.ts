import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { readFileSync } from "node:fs";
const handlers = vi.hoisted(() => new Map<string, (m: any) => void>());
vi.mock("../utils/host-messages", () => ({
  onHostMessage: (type: string, handler: any) => handlers.set(type, handler),
}));
class Element {
  value = "";
  textContent = "";
  dataset: any = {};
  checked = false;
  children: Element[] = [];
  onclick: any;
  onchange: any;
  events = new Map<string, any>();
  className = "";
  classList = { add: vi.fn() };
  addEventListener(n: string, fn: any) {
    this.events.set(n, fn);
  }
  append(...rows: Element[]) {
    this.children.push(...rows);
  }
  prepend(row: Element) {
    this.children.unshift(row);
  }
  replaceChildren(...rows: Element[]) {
    this.children = rows;
  }
  add(row: Element) {
    this.children.push(row);
  }
  get options() {
    return this.children;
  }
  get lastElementChild() {
    return this.children.at(-1);
  }
  remove() {}
  querySelector() {
    return this.children.find((c) => c.textContent.includes("尚未发送"));
  }
  querySelectorAll() {
    return this.children.filter((c) => c.dataset.key);
  }
  click() {
    this.onclick?.();
  }
}
let elements: Map<string, Element>, sent: any[];
const get = (id: string) => elements.get(id)!;
const message = (id: string, speaker = "同事", text = "见面聊聊") => ({
  id,
  speaker,
  text,
  time: "",
  self: false,
  source: "uia",
});
const windowRow = (title = "项目群", window = "process-1") => ({
  window,
  platform: "wechat",
  title,
  kind: "group",
  status: "可读",
  readable: true,
  messages: [message("1")],
});
function respond(data: any = {}, type = "chatResponse", requestId?: string) {
  const req = sent
    .filter(
      (m) =>
        m.type ===
        (type === "chatResponse" ? "chatRequest" : "assistantRequest"),
    )
    .at(-1).data;
  handlers.get(type)?.({
    requestId: requestId ?? req.requestId,
    action: req.action,
    ok: true,
    message: "ok",
    ...data,
  });
}
function click(id: string) {
  get(id).click();
}
function scanBind() {
  click("crScan");
  respond({ windows: [windowRow()] });
  get("crWindows").value = "process-1\n项目群";
  get("crAccount").value = "工作";
  get("crSelf").value = "我";
  get("crKey").value = "project";
  get("crKind").value = "group";
  click("crBind");
}
function event(revision = 2, speaker = "同事") {
  return {
    key: "project",
    title: "项目群",
    incoming: speaker ? [message(String(revision), speaker)] : [],
    revision,
  };
}
function poll(events: any[] = []) {
  vi.advanceTimersByTime(3000);
  respond({ windows: [windowRow()], sessions: [], events });
}
function generate() {
  poll([event()]);
  vi.advanceTimersByTime(3000);
  respond({
    session: {
      key: "project",
      title: "项目群",
      kind: "group",
      platform: "wechat",
      account: "工作",
      messages: [message("1", "我", "以前的约定"), message("2")],
    },
  });
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
  elements = new Map(
    [...html.matchAll(/id="([^"]+)"/g)].map((m) => [m[1], new Element()]),
  );
  get("rsMode").value = "high_eq";
  get("rsCount").value = "3";
  vi.stubGlobal("document", {
    getElementById: (id: string) => get(id),
    createElement: () => new Element(),
  });
  vi.stubGlobal("window", {
    chrome: {
      webview: { postMessage: (s: string) => sent.push(JSON.parse(s)) },
    },
    setTimeout,
    clearTimeout,
    setInterval,
    addEventListener: vi.fn(),
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
  const mod = await import("./chat-reader");
  mod.setupChatReader();
  respond({ sessions: [] });
});
afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});
it("does not scan or send model content before explicit start", () => {
  vi.advanceTimersByTime(20000);
  expect(sent).toHaveLength(1);
  expect(sent[0].data.action).toBe("overview");
});
it("binds the stable window identity and records only after start", () => {
  scanBind();
  click("crStart");
  poll();
  const req = sent.at(-1).data;
  expect(req.action).toBe("poll");
  expect(req.bindings[0]).toMatchObject({
    key: "project",
    account: "工作",
    title: "项目群",
    kind: "group",
  });
  expect(sent.some((m) => m.type === "assistantRequest")).toBe(false);
});
it("does not generate on baseline or when automatic replies are disabled", () => {
  scanBind();
  click("crStart");
  poll([event()]);
  vi.advanceTimersByTime(3000);
  expect(sent.some((m) => m.type === "assistantRequest")).toBe(false);
});
it("sends only the triggering conversation to the separate model queue and keeps polling", () => {
  scanBind();
  click("crStart");
  get("crAuto").checked = true;
  generate();
  const req = sent.at(-1);
  expect(req.type).toBe("assistantRequest");
  expect(req.data).toMatchObject({ origin: "chat", count: 3, mode: "high_eq" });
  expect(req.data.context).toContain("以前的约定");
  expect(req.data.context).not.toContain("见面聊聊");
  expect(req.data.incoming).toContain("见面聊聊");
  vi.advanceTimersByTime(3000);
  expect(sent.at(-1).data.action).toBe("poll");
});
it("drops a late model response after an own message arrives", () => {
  scanBind();
  click("crStart");
  get("crAuto").checked = true;
  generate();
  const id = sent.at(-1).data.requestId;
  poll([event(3, "")]);
  respond(
    { replies: [{ label: "旧", text: "过时内容" }] },
    "assistantResponse",
    id,
  );
  expect(get("crReplies").children).toHaveLength(0);
  expect(
    sent.some(
      (m) =>
        m.type === "assistantRequest" &&
        m.data.action === "cancel" &&
        m.data.origin === "chat",
    ),
  ).toBe(true);
});
it("stop cancels pending generation and cannot send a chat message", () => {
  scanBind();
  click("crStart");
  get("crAuto").checked = true;
  generate();
  const id = sent.at(-1).data.requestId;
  click("crStart");
  respond(
    { replies: [{ label: "旧", text: "不该显示" }] },
    "assistantResponse",
    id,
  );
  expect(get("crReplies").children).toHaveLength(0);
  expect(get("crAuto").checked).toBe(false);
  expect(
    sent.every((m) => ["chatRequest", "assistantRequest"].includes(m.type)),
  ).toBe(true);
});
it("renders alternatives literally and copying requires a click", () => {
  scanBind();
  click("crStart");
  get("crAuto").checked = true;
  generate();
  respond(
    { replies: [{ label: "建议", text: "<script>never execute</script>" }] },
    "assistantResponse",
  );
  expect(get("crReplies").children).toHaveLength(1);
  expect(sent.some((m) => m.type === "copyText")).toBe(false);
  const card = get("crReplies").children[0];
  expect(card.children.some((c) => c.textContent.includes("<script>"))).toBe(
    true,
  );
  card.children.at(-1)!.click();
  expect(sent.at(-1)).toMatchObject({
    type: "copyText",
    data: "<script>never execute</script>",
  });
});
it("does not keep a selection when a different conversation replaces the window", () => {
  scanBind();
  click("crScan");
  respond({ windows: [windowRow("另一个群")] });
  expect(
    get("crWindows").children.some((o) => o.value === "process-1\n项目群"),
  ).toBe(false);
});
it("bounds model history by UTF-8 bytes without breaking messages", async () => {
  const { chatContext } = await import("./chat-reader");
  const result = chatContext(
    [message("1", "甲", "旧".repeat(200)), message("2", "乙", "新消息")],
    120,
  );
  expect(result).toContain("新消息");
  expect(result).not.toContain("旧");
  expect(new TextEncoder().encode(result).length).toBeLessThanOrEqual(120);
});

it("hiding the native window stops polling and discards late model results", () => {
  scanBind();
  click("crStart");
  const handler = (window.addEventListener as any).mock.calls.find(
    (c: any[]) => c[0] === "msime-chat-hidden",
  )[1];
  handler();
  expect(get("crStart").textContent).toBe("开始只读监测");
  expect(get("crAuto").checked).toBe(false);
});
