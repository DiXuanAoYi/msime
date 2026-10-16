import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import { bindQuickControls, setSeg } from "./quick-controls";

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

const BENCH_TABS = new Set(["image", "text", "conversation", "subtitles", "services"]);

export function setupVisionQuick(): void {
  const root = document.getElementById("visionPanel");
  if (!root || root.dataset.quickReady) return;
  root.dataset.quickReady = "1";
  bindQuickControls(root);

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
    if (!jump) return;
    setSeg(root, "vision", "bench");
    const tab = jump.dataset.bench!;
    if (BENCH_TABS.has(tab)) root.querySelector<HTMLElement>(`[data-vision-tab="${tab}"]`)?.click();
  });
  render();
}
