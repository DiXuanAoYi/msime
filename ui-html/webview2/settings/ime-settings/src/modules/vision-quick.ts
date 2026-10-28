import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import { cloudOcrConnected, niuConnected, tencentTmtConnected, textModel, textModelChip } from "./connections";
import { bindQuickControls, type ChipResolver, type ChipState, type RawReader } from "./quick-controls";

export interface VisionHistoryRow {
  id: string;
  at: number;
  kind: string;
  text: string;
  translation: string;
}

const KIND_LABEL: Record<string, string> = { ocr: "识字", translate: "截图翻译", selection: "划词翻译" };

export function historyKindLabel(kind: string): string {
  return KIND_LABEL[kind] ?? "记录";
}

/** "刚刚 / 12 分钟前 / 3 小时前 / 5 月 3 日" for a millisecond timestamp. */
export function historyAge(at: number, now = Date.now()): string {
  if (!at) return "";
  const minutes = Math.max(0, Math.floor((now - at) / 60000));
  if (minutes < 1) return "刚刚";
  if (minutes < 60) return `${minutes} 分钟前`;
  if (minutes < 1440) return `${Math.floor(minutes / 60)} 小时前`;
  const date = new Date(at);
  return `${date.getMonth() + 1} 月 ${date.getDate()} 日`;
}

export function filterHistory(rows: VisionHistoryRow[], kind: string, query: string): VisionHistoryRow[] {
  const needle = query.trim().toLowerCase();
  return rows.filter((row) => {
    if (kind !== "all" && row.kind !== kind) return false;
    return !needle || row.text.toLowerCase().includes(needle) || row.translation.toLowerCase().includes(needle);
  });
}

const CLOUD_OCR = new Set(["shiliu", "tencent", "baidu"]);
const BENCH_TABS = new Set(["image", "text", "conversation", "subtitles", "services"]);

const LINE_SAMPLE =
  "Offline sync keeps a full copy of your notes on this device. Changes you make without a connection are queued and uploaded automatically the next time you go online.";
const LINE_SAMPLE_2 = "Before you begin, make sure at least 2 GB of free disk space is available.";

function wrapLines(text: string, width: number): string {
  const lines: string[] = [];
  let line = "";
  for (const word of text.split(" ")) {
    if (line && line.length + word.length + 1 > width) {
      lines.push(line);
      line = word;
    } else {
      line = line ? `${line} ${word}` : word;
    }
  }
  if (line) lines.push(line);
  return lines.join("\n");
}

/** What the "换行" option does to a screen paragraph, shown under the option. */
export function linePreview(mode: string): string {
  const two = `${LINE_SAMPLE}\n\n${LINE_SAMPLE_2}`;
  if (mode === "keep") return two.split("\n\n").map((paragraph) => wrapLines(paragraph, 48)).join("\n\n");
  if (mode === "one") return two.replace(/\n+/g, " ");
  return two;
}

/** Local OCR model state as the "识字" tab shows it. */
export interface LocalOcrState {
  running: boolean;
  downloaded: boolean;
}

type Value = (path: string) => string | undefined;

function translationChip(value: Value, raw: RawReader): ChipState {
  const provider = value("vision.translation_provider") || "text_model";
  if (provider === "dictionary") return { text: "翻译：本机词典（只能查词）", tone: "warn" };
  if (provider === "niutrans") {
    return niuConnected(value) ? { text: "翻译：小牛翻译", tone: "good" } : { text: "翻译：小牛翻译（未连接）", tone: "warn" };
  }
  if (provider === "tencent") {
    return tencentTmtConnected(value)
      ? { text: "翻译：腾讯翻译", tone: "good" }
      : { text: "翻译：腾讯翻译（未连接）", tone: "warn" };
  }
  const model = textModel(value, raw, value("vision.text_provider") || "");
  return { text: `翻译：${model.label}${model.connected ? "" : "（未连接）"}`, tone: model.connected ? "good" : "warn" };
}

export function visionChips(local: () => LocalOcrState): Record<string, ChipResolver> {
  return {
    visionOcr: (value) => {
      const provider = value("vision.provider") || "smart";
      if (CLOUD_OCR.has(provider)) {
        return cloudOcrConnected(value)
          ? { text: "识字：云端", tone: "good" }
          : { text: "识字：云端（未连接）", tone: "warn" };
      }
      if (provider === "windows") return { text: "识字：Windows 本机", tone: "good" };
      if (provider === "local") return { text: "识字：本地模型", tone: local().downloaded ? "good" : "warn" };
      return { text: "识字：智能", tone: "good" };
    },
    visionTr: translationChip,
    ocrSmart: () =>
      local().downloaded ? { text: "可用", tone: "good" } : { text: "本地模型未下载，先用 Windows", tone: "warn" },
    ocrLocal: () => (local().downloaded ? { text: "已下载", tone: "good" } : { text: "未下载", tone: "off" }),
    ocrCloud: (value) => (cloudOcrConnected(value) ? { text: "已连接", tone: "good" } : { text: "未连接", tone: "off" }),
    ocrService: () => (local().running ? { text: "运行中", tone: "good" } : { text: "未运行", tone: "off" }),
    trAi: (value, raw) => textModelChip(value, raw, value("vision.text_provider") || ""),
    trNiu: (value) => (niuConnected(value) ? { text: "已连接", tone: "good" } : { text: "未连接", tone: "off" }),
    trTencent: (value) => (tencentTmtConnected(value) ? { text: "已连接", tone: "good" } : { text: "未连接", tone: "off" }),
  };
}

export function visionDerived(value: Value): Record<string, string> {
  const provider = value("vision.provider") || "smart";
  const translation = value("vision.translation_provider") || "text_model";
  const cloud = CLOUD_OCR.has(provider);
  return {
    "vision.cloud_selected": String(cloud),
    "vision.cloud_missing": String(cloud && !cloudOcrConnected(value)),
    "vision.dict_selected": String(translation === "dictionary"),
    "vision.ai_selected": String(translation === "text_model"),
  };
}

/** Opens the 翻译工具 page and, once it has loaded, the tab the caller asked for. */
export function openBenchTab(tab: string): void {
  document.querySelector<HTMLElement>('.sidebar .item[data-target="vision-bench"]')?.click();
  if (!BENCH_TABS.has(tab)) return;
  let tries = 0;
  const timer = window.setInterval(() => {
    const button = document.querySelector<HTMLElement>(`#visionPanel [data-vision-tab="${tab}"]`);
    if (button || ++tries > 40) {
      window.clearInterval(timer);
      button?.click();
    }
  }, 50);
}

export function setupVisionQuick(): void {
  const root = document.getElementById("visionSettings");
  if (!root || root.dataset.quickReady) return;
  root.dataset.quickReady = "1";
  const local: LocalOcrState = { running: false, downloaded: false };
  const preview = root.querySelector<HTMLElement>("#visionLinePreview");
  const controls = bindQuickControls(
    root,
    (path, value) => {
      if (path === "vision.line_mode" && preview) preview.textContent = linePreview(value);
    },
    visionChips(() => local),
    visionDerived,
  );
  const syncPreview = () => {
    if (preview) preview.textContent = linePreview(controls.value("vision.line_mode") || "smart");
  };
  syncPreview();

  let rows: VisionHistoryRow[] = [];
  let kind = "all";
  const grid = root.querySelector<HTMLElement>("#visionHistGrid")!;
  const query = root.querySelector<HTMLInputElement>("#visionHistQuery")!;

  const send = (action: string, id?: string) =>
    window.chrome?.webview?.postMessage(
      serializeHostMessage({
        type: "visionRequest",
        data: { requestId: `vq-${action}-0`, action: action as "history", ...(id ? { id } : {}) },
      }),
    );

  window.chrome?.webview?.postMessage(
    serializeHostMessage({ type: "localOcrRequest", data: { requestId: "vq-local-ocr", action: "status" } }),
  );

  const copy = async (text: string) => {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      // The clipboard can be refused while the window is unfocused; the text stays on screen.
    }
  };

  const render = () => {
    const shown = filterHistory(rows, kind, query.value);
    grid.replaceChildren();
    if (!shown.length) {
      const empty = document.createElement("div");
      empty.className = "empty";
      empty.textContent = rows.length ? "没有匹配的记录。" : "还没有历史记录。截图识字或翻译之后，结果会出现在这里。";
      grid.append(empty);
      return;
    }
    for (const row of shown) {
      const card = document.createElement("article");
      card.className = "hc";
      const body = document.createElement("div");
      body.className = "hc-body";
      const meta = document.createElement("div");
      meta.className = "hc-meta";
      const chip = document.createElement("span");
      chip.className = "chip accent";
      chip.textContent = historyKindLabel(row.kind);
      const age = document.createElement("span");
      age.textContent = historyAge(row.at);
      meta.append(chip, age);
      const text = document.createElement("div");
      text.className = "hc-text";
      text.textContent = row.translation || row.text;
      const source = document.createElement("div");
      source.className = "hc-text muted";
      source.textContent = row.translation ? row.text : "";
      source.hidden = !row.translation;
      const actions = document.createElement("div");
      actions.className = "hc-acts";
      const button = (label: string, run: () => void, danger = false) => {
        const b = document.createElement("button");
        b.type = "button";
        b.className = `btn sm ghost${danger ? " danger" : ""}`;
        b.textContent = label;
        b.onclick = run;
        return b;
      };
      actions.append(
        button(row.translation ? "复制译文" : "复制文字", () => void copy(row.translation || row.text)),
        ...(row.translation ? [button("复制原文", () => void copy(row.text))] : []),
        button("删除", () => send("historyDelete", row.id), true),
      );
      body.append(meta, text, source, actions);
      card.append(body);
      grid.append(card);
    }
  };

  onHostMessage("visionResponse", (message) => {
    if (!message.requestId.startsWith("vq-")) return;
    if (Array.isArray(message.history)) {
      rows = message.history as unknown as VisionHistoryRow[];
      render();
    }
  });
  onHostMessage("localOcrResponse", (message) => {
    if (message.type !== "localOcrResponse") return;
    const models = Array.isArray(message.models)
      ? (message.models as unknown as Array<{ status?: { state?: string } }>)
      : [];
    local.downloaded = models.some((row) => ["ready", "installed", "downloaded"].includes(String(row.status?.state ?? "")));
    local.running = message.service?.state === "running";
    controls.refresh();
  });

  query.addEventListener("input", render);
  root.querySelector("#visionHistFilter")?.addEventListener("click", (event) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>("button[data-v]");
    if (!button) return;
    kind = button.dataset.v!;
    root.querySelectorAll<HTMLElement>("#visionHistFilter button").forEach((b) => {
      b.setAttribute("aria-pressed", String(b === button));
    });
    render();
  });
  root.querySelector("#visionHistClear")?.addEventListener("click", () => {
    if (rows.length && window.confirm("清空全部识字和翻译历史？此操作无法撤销。")) send("historyClear");
  });
  root.addEventListener("qp-tab", (event) => {
    const detail = (event as CustomEvent<{ name: string; tab: string }>).detail;
    if (detail.name === "vision" && detail.tab === "history") send("history");
  });

  root.addEventListener("click", (event) => {
    const jump = (event.target as HTMLElement).closest<HTMLElement>("[data-bench]");
    if (jump) openBenchTab(jump.dataset.bench!);
  });
  render();
}
