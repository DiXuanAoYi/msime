import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import { updateConfig } from "./config-sync";
import { asrProviderKey, textModelLabel, textProviderKey, TEXT_PROVIDERS } from "./connections";
import { POLISH_DEFAULTS } from "./voice";
import "../styles/modules/quick-pages.css";

/**
 * 服务连接: one place to fill in every key. It edits the same config keys the feature pages already use, so nothing
 * moves and the old forms keep working. A text-model key is also written to the AI 联想 and 候选词翻译 key slots of the
 * providers they share, which is what "fill it once" means for those two pages.
 */

type Snapshot = Record<string, Record<string, unknown> | undefined>;
type Group = "text" | "asr" | "translate" | "ocr";

interface Field {
  id: string;
  label: string;
  secret: boolean;
  wide?: boolean;
  read: (s: Snapshot) => string;
  write: string[];
}

interface Service {
  id: string;
  group: Group;
  name: string;
  detail: string;
  fields: Field[];
  /** Extra writes after the fields are saved (e.g. switching a translator on). */
  after?: Array<[string, string | boolean]>;
  test?: () => { service: string; config: Record<string, string> } | null;
}

const GROUP_LABEL: Record<Group, string> = { text: "文本模型", asr: "语音识别", translate: "翻译服务", ocr: "文字识别" };
const SHARED_WITH_AI = new Set(["deepseek", "openai", "siliconflow", "groq"]);

let snapshot: Snapshot = {};
let expanded = "";
let drawer: HTMLElement | null = null;
const testState = new Map<string, { text: string; ok: boolean }>();
let pendingTest = "";

const text = (s: Snapshot, section: string, key: string): string => {
  const value = s[section]?.[key];
  return typeof value === "string" ? value.trim() : value === undefined || value === null ? "" : String(value);
};
const reader = (s: Snapshot) => ({
  value: (path: string) => {
    const [section, key] = path.split(".");
    return text(s, section, key) || undefined;
  },
  raw: (path: string) => {
    const [section, key] = path.split(".");
    return s[section]?.[key];
  },
});

function services(): Service[] {
  const list: Service[] = [];
  for (const id of TEXT_PROVIDERS) {
    const defaults = POLISH_DEFAULTS[id];
    list.push({
      id: `text-${id}`,
      group: "text",
      name: textModelLabel(id),
      detail: `文本模型 · ${defaults?.model ?? ""}`,
      fields: [
        {
          id: "key",
          label: "API Key",
          secret: true,
          wide: true,
          read: (s) => {
            const r = reader(s);
            return textProviderKey(r.value, r.raw, id);
          },
          write: [
            `voice_input.polish_token_${id}`,
            ...(SHARED_WITH_AI.has(id) ? [`ai_assistant.token_${id}`, `candidate_translation.token_${id}`] : []),
          ],
        },
      ],
      test: () => {
        const token = list.find((item) => item.id === `text-${id}`) && currentInput(`text-${id}`, "key");
        return {
          service: "voice.polish",
          config: { provider: id, token: token ?? "", endpoint: defaults?.endpoint ?? "", model: defaults?.model ?? "" },
        };
      },
    });
  }
  const asr: Array<[string, string, string]> = [
    ["doubao", "豆包语音", "语音识别 · 流式"],
    ["openai", "OpenAI 语音识别", "语音识别"],
    ["siliconflow", "硅基流动 语音识别", "语音识别"],
    ["groq", "Groq 语音识别", "语音识别"],
    ["aliyun", "阿里云百炼 语音识别", "语音识别 · Paraformer"],
  ];
  for (const [id, name, detail] of asr) {
    list.push({
      id: `asr-${id}`,
      group: "asr",
      name,
      detail,
      fields: [
        {
          id: "key",
          label: "API Key",
          secret: true,
          wide: true,
          read: (s) => {
            const r = reader(s);
            return asrProviderKey(r.value, r.raw, id);
          },
          write: [`voice_input.asr_token_${id}`],
        },
      ],
    });
  }
  list.push({
    id: "asr-tencent",
    group: "asr",
    name: "腾讯云 语音识别",
    detail: "语音识别 · 实时",
    fields: [
      { id: "id", label: "SecretId", secret: true, read: (s) => text(s, "voice_input", "tencent_secret_id"), write: ["voice_input.tencent_secret_id"] },
      {
        id: "key",
        label: "SecretKey",
        secret: true,
        read: (s) => {
          const r = reader(s);
          return asrProviderKey(r.value, r.raw, "tencent");
        },
        write: ["voice_input.asr_token_tencent"],
      },
    ],
  });
  list.push({
    id: "tr-niutrans",
    group: "translate",
    name: "小牛翻译",
    detail: "机器翻译 · 按字符计费",
    fields: [
      { id: "app", label: "App ID", secret: false, read: (s) => text(s, "niutrans", "app_id"), write: ["niutrans.app_id"] },
      { id: "key", label: "API Key", secret: true, read: (s) => text(s, "niutrans", "apikey"), write: ["niutrans.apikey"] },
    ],
    after: [["niutrans.enabled", true]],
  });
  list.push({
    id: "tr-tencent",
    group: "translate",
    name: "腾讯翻译",
    detail: "机器翻译 · 仅已开通的老帐号",
    fields: [
      { id: "id", label: "SecretId", secret: true, read: (s) => text(s, "tencent_tmt", "secret_id"), write: ["tencent_tmt.secret_id"] },
      { id: "key", label: "SecretKey", secret: true, read: (s) => text(s, "tencent_tmt", "secret_key"), write: ["tencent_tmt.secret_key"] },
    ],
  });
  list.push({
    id: "ocr-shiliu",
    group: "ocr",
    name: "石榴智能 OCR",
    detail: "云端文字识别",
    fields: [
      { id: "code", label: "APPCODE", secret: true, wide: true, read: (s) => text(s, "vision", "shiliu_appcode"), write: ["vision.shiliu_appcode"] },
    ],
  });
  list.push({
    id: "ocr-tencent",
    group: "ocr",
    name: "腾讯云 OCR",
    detail: "云端文字识别",
    fields: [
      { id: "id", label: "SecretId", secret: true, read: (s) => text(s, "vision", "tencent_secret_id"), write: ["vision.tencent_secret_id"] },
      { id: "key", label: "SecretKey", secret: true, read: (s) => text(s, "vision", "tencent_secret_key"), write: ["vision.tencent_secret_key"] },
    ],
  });
  list.push({
    id: "ocr-baidu",
    group: "ocr",
    name: "百度智能云 OCR",
    detail: "云端文字识别",
    fields: [
      { id: "key", label: "API Key", secret: true, read: (s) => text(s, "vision", "baidu_api_key"), write: ["vision.baidu_api_key"] },
      { id: "secret", label: "Secret Key", secret: true, read: (s) => text(s, "vision", "baidu_secret_key"), write: ["vision.baidu_secret_key"] },
    ],
  });
  return list;
}

export const isConnected = (service: Service, s: Snapshot): boolean => service.fields.every((field) => field.read(s) !== "");

function currentInput(serviceId: string, fieldId: string): string {
  return drawer?.querySelector<HTMLInputElement>(`input[data-s="${serviceId}"][data-f="${fieldId}"]`)?.value.trim() ?? "";
}

const el = <K extends keyof HTMLElementTagNameMap>(tag: K, className = "", content = ""): HTMLElementTagNameMap[K] => {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (content) node.textContent = content;
  return node;
};

function chip(label: string, tone: string): HTMLElement {
  return el("span", `chip ${tone}`, label);
}

function connectionRow(service: Service): HTMLElement {
  const row = el("div", "conn");
  const main = el("div", "conn-main");
  main.append(el("b", "", service.name), el("small", "", `${GROUP_LABEL[service.group]} · ${service.detail.split(" · ").slice(1).join(" · ") || service.detail}`));
  const connected = isConnected(service, snapshot);
  const edit = el("button", "btn sm", expanded === service.id ? "收起" : connected ? "编辑" : "连接");
  edit.type = "button";
  edit.onclick = () => {
    expanded = expanded === service.id ? "" : service.id;
    render();
  };
  row.append(main, chip(connected ? "已连接" : "未连接", connected ? "good" : "off"), edit);
  if (expanded === service.id) row.append(connectionForm(service));
  return row;
}

function connectionForm(service: Service): HTMLElement {
  const form = el("div", "conn-form");
  for (const field of service.fields) {
    const label = el("label", `field${field.wide || service.fields.length === 1 ? " wide" : ""}`);
    label.append(field.label);
    const input = el("input");
    input.type = field.secret ? "password" : "text";
    input.autocomplete = "off";
    input.dataset.s = service.id;
    input.dataset.f = field.id;
    input.value = field.read(snapshot);
    label.append(input);
    form.append(label);
  }
  const actions = el("div", "row-r left wide");
  const save = el("button", "btn sm primary", "保存");
  save.type = "button";
  const status = el("span", "small test-st");
  const known = testState.get(service.id);
  if (known) {
    status.textContent = known.text;
    status.className = `small test-st ${known.ok ? "good" : "bad"}`;
  }
  save.onclick = () => {
    for (const field of service.fields) {
      const value = currentInput(service.id, field.id);
      if (value === field.read(snapshot)) continue;
      for (const path of field.write) updateConfig(path, value);
      const [section, key] = field.write[0].split(".");
      snapshot = { ...snapshot, [section]: { ...(snapshot[section] ?? {}), [key]: value } };
    }
    for (const [path, value] of service.after ?? []) updateConfig(path, value);
    status.textContent = "已保存";
    status.className = "small test-st good";
    window.setTimeout(() => window.chrome?.webview?.postMessage(serializeHostMessage({ type: "configRequest" })), 400);
  };
  actions.append(save);
  if (service.test) {
    const test = el("button", "btn sm", "测试");
    test.type = "button";
    test.onclick = () => {
      const request = service.test!();
      if (!request || !request.config.token) {
        status.textContent = "先填写 API Key。";
        status.className = "small test-st bad";
        return;
      }
      pendingTest = `conn-${service.id}-${Date.now()}`;
      status.textContent = "正在连接服务…";
      status.className = "small test-st";
      window.chrome?.webview?.postMessage(
        serializeHostMessage({
          type: "apiCredentialTest",
          data: { requestId: pendingTest, service: request.service as "voice.polish", config: request.config },
        }),
      );
      testState.set(service.id, { text: "正在连接服务…", ok: true });
    };
    actions.append(test);
  }
  actions.append(status);
  form.append(actions);
  return form;
}

function usageRow(label: string, path: string, options: string[], allowFollow: boolean): HTMLElement {
  const row = el("div", "row");
  const left = el("div", "row-l");
  left.append(el("b", "", label));
  const right = el("div", "row-r");
  const select = el("select");
  select.setAttribute("aria-label", label);
  const [section, key] = path.split(".");
  const current = text(snapshot, section, key);
  const choices = new Set(options);
  if (current) choices.add(current);
  if (allowFollow) select.add(new Option("跟随润色设置", ""));
  for (const id of choices) select.add(new Option(textModelLabel(id), id));
  select.value = current;
  select.onchange = () => {
    updateConfig(path, select.value);
    snapshot = { ...snapshot, [section]: { ...(snapshot[section] ?? {}), [key]: select.value } };
    render();
  };
  right.append(select);
  row.append(left, right);
  return row;
}

function render(): void {
  if (!drawer) return;
  const body = drawer.querySelector<HTMLElement>(".drawer-body")!;
  body.replaceChildren();
  const all = services();
  const connected = all.filter((service) => isConnected(service, snapshot) || service.id === expanded);
  const missing = all.filter((service) => !connected.includes(service));

  const have = el("div", "card");
  const head = el("div", "card-h");
  head.append(el("h2", "", "已连接"));
  have.append(head);
  if (!connected.length) have.append(el("p", "small muted", "还没有连接任何服务。从下面的“添加连接”开始。"));
  connected.forEach((service) => have.append(connectionRow(service)));
  body.append(have);

  const usage = el("div", "card");
  const usageHead = el("div", "card-h");
  usageHead.append(el("h2", "", "各功能使用"), el("p", "", "默认都用识别后润色选的那一个文本模型，也可以单独指定。"));
  usage.append(usageHead);
  const ready = TEXT_PROVIDERS.filter((id) => textProviderKey(reader(snapshot).value, reader(snapshot).raw, id) !== "");
  usage.append(
    usageRow("识别后润色", "voice_input.polish_provider", [...ready], false),
    usageRow("截图翻译 · 划词翻译", "vision.text_provider", [...ready], true),
    usageRow("嘴替 · 回复", "reply.provider", [...ready], true),
  );
  const note = el("div", "row");
  const noteLeft = el("div", "row-l");
  noteLeft.append(
    el("b", "", "AI 联想 · 候选词翻译"),
    el("small", "", `现在用 ${textModelLabel(text(snapshot, "ai_assistant", "provider") || "deepseek")} 和 ${textModelLabel(text(snapshot, "candidate_translation", "ai_provider") || "deepseek")}；提供商和模型在“AI 辅助”“输入”页里选，密钥在这里填一次就同步。`),
  );
  note.append(noteLeft);
  usage.append(note);
  body.append(usage);

  if (missing.length) {
    const add = el("div", "card");
    const addHead = el("div", "card-h");
    addHead.append(el("h2", "", "添加连接"));
    add.append(addHead);
    const grid = el("div", "addgrid");
    for (const service of missing) {
      const button = el("button");
      button.type = "button";
      button.innerHTML = '<svg class="ic"><use href="#i-plus"/></svg>';
      button.append(service.name);
      button.onclick = () => {
        expanded = service.id;
        render();
        drawer?.querySelector(`input[data-s="${service.id}"]`)?.scrollIntoView({ block: "center" });
      };
      grid.append(button);
    }
    add.append(grid);
    body.append(add);
  }
}

function build(): HTMLElement {
  const root = el("div", "qp drawer");
  root.id = "connectionsDrawer";
  root.hidden = true;
  const panel = el("div", "drawer-panel");
  panel.setAttribute("role", "dialog");
  panel.setAttribute("aria-modal", "true");
  panel.setAttribute("aria-label", "服务连接");
  const head = el("div", "dr-head");
  const title = el("div");
  title.append(el("h2", "", "服务连接"), el("p", "lead", "每个服务的密钥只填一次，各功能按需选用。密钥只保存在本机。"));
  const close = el("button", "icon-btn");
  close.type = "button";
  close.setAttribute("aria-label", "关闭");
  close.innerHTML = '<svg class="ic"><use href="#i-x"/></svg>';
  close.onclick = closeConnections;
  head.append(title, close);
  const body = el("div", "drawer-body");
  panel.append(head, body);
  root.append(panel);
  root.addEventListener("mousedown", (event) => {
    if (event.target === root) closeConnections();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !root.hidden) closeConnections();
  });
  return root;
}

export function closeConnections(): void {
  if (drawer) drawer.hidden = true;
}

export function openConnections(service = ""): void {
  if (!drawer) {
    drawer = build();
    document.body.append(drawer);
    onHostMessage("configSnapshot", (message) => {
      snapshot = message.data as unknown as Snapshot;
      if (!drawer?.hidden && !drawer?.contains(document.activeElement)) render();
    });
    onHostMessage("apiCredentialTestResult", (message) => {
      if (!pendingTest || message.requestId !== pendingTest) return;
      const id = pendingTest.split("-").slice(1, -1).join("-");
      pendingTest = "";
      testState.set(id, { text: message.message, ok: message.ok });
      render();
    });
  }
  expanded = service;
  drawer.hidden = false;
  render();
  window.chrome?.webview?.postMessage(serializeHostMessage({ type: "configRequest" }));
}
