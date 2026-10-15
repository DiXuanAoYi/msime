import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import { updateConfig } from "./config-sync";
import { personalAction } from "./personal-library";
import { setupLocalOcr } from "./local-ocr";
import "../styles/modules/vision.css";
const el = <T extends HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const value = (id: string) => el<HTMLInputElement>(id).value;
const languages = [
  ["auto", "自动检测"],
  ["zh", "中文"],
  ["en", "英语"],
  ["ja", "日语"],
  ["ko", "韩语"],
  ["fr", "法语"],
  ["de", "德语"],
  ["es", "西班牙语"],
  ["ru", "俄语"],
  ["ar", "阿拉伯语"],
  ["pt", "葡萄牙语"],
  ["it", "意大利语"],
  ["th", "泰语"],
  ["vi", "越南语"],
  ["bo", "藏语"],
  ["ug", "维吾尔语"],
  ["mn", "蒙古语"],
];
type Action =
  | "status"
  | "stop"
  | "finish"
  | "saveImage"
  | "import"
  | "recognize"
  | "translate"
  | "capture"
  | "longCapture"
  | "screen"
  | "subtitles"
  | "importSubtitles"
  | "translateSubtitles";
export interface SubtitleSegment {
  id: string;
  start: number;
  end: number;
  text: string;
  translation: string;
  dirtyText?: boolean;
  dirtyTranslation?: boolean;
}
interface SubtitleDraft { segments: SubtitleSegment[]; editor: string; dirty: boolean }
export function subtitleTime(seconds: number): string {
  const ms = Math.max(0, Math.round(Number.isFinite(seconds) ? seconds * 1000 : 0));
  return `${Math.floor(ms / 3600000).toString().padStart(2, "0")}:${Math.floor(ms / 60000 % 60).toString().padStart(2, "0")}:${Math.floor(ms / 1000 % 60).toString().padStart(2, "0")},${(ms % 1000).toString().padStart(3, "0")}`;
}
export function renderSubtitleSrt(segments: SubtitleSegment[], translated = true, offset = 0): string {
  return segments.map((segment, index) => `${index + offset + 1}\n${subtitleTime(segment.start)} --> ${subtitleTime(segment.end)}\n${segment.text}${translated && segment.translation ? "\n" + segment.translation : ""}\n\n`).join("");
}
export function mergeSubtitleSegments(current: SubtitleSegment[], incoming: unknown): SubtitleSegment[] {
  if (!Array.isArray(incoming)) return current;
  const next = current.map((x) => ({ ...x }));
  const positions = new Map(next.map((x, index) => [x.id, index]));
  for (const [index, raw] of incoming.entries()) {
    if (!raw || typeof raw !== "object" || typeof raw.text !== "string" ||
      !Number.isFinite(raw.start) || !Number.isFinite(raw.end) || raw.start < 0 || raw.end < raw.start) continue;
    const id = typeof raw.id === "string" ? raw.id : `segment-${index + 1}`;
    const oldIndex = positions.get(id);
    const old = oldIndex === undefined ? undefined : next[oldIndex];
    const merged: SubtitleSegment = {
      id, start: raw.start, end: raw.end,
      text: old?.dirtyText ? old.text : raw.text,
      translation: old?.dirtyTranslation || (old?.dirtyText && old.text !== raw.text)
        ? old.translation : typeof raw.translation === "string" ? raw.translation : "",
      dirtyText: old?.dirtyText, dirtyTranslation: old?.dirtyTranslation,
    };
    if (oldIndex === undefined) {
      positions.set(id, next.length);
      next.push(merged);
    } else next[oldIndex] = merged;
  }
  return next;
}
export function appendFreshSubtitleCues(editor: string, previous: SubtitleSegment[], next: SubtitleSegment[]): string {
  const oldIds = new Set(previous.map((x) => x.id));
  const fresh = next.filter((x) => !oldIds.has(x.id));
  if (!fresh.length) return editor;
  const separator = !editor || editor.endsWith("\n\n") ? "" : editor.endsWith("\n") ? "\n" : "\n\n";
  return editor + separator + renderSubtitleSrt(fresh, true, previous.length);
}
export function recoverSubtitleFailure(current: SubtitleSegment[], incoming: unknown, backup: SubtitleSegment[]): { segments: SubtitleSegment[]; restored: boolean } {
  const received = mergeSubtitleSegments([], incoming);
  if (received.length || current.length) return { segments: mergeSubtitleSegments(current, received), restored: false };
  return { segments: backup.map((x) => ({ ...x })), restored: true };
}
export function decodeSubtitleBytes(bytes: Uint8Array): string {
  if (bytes[0] === 0xff && bytes[1] === 0xfe) return new TextDecoder("utf-16le").decode(bytes);
  if (bytes[0] === 0xfe && bytes[1] === 0xff) return new TextDecoder("utf-16be").decode(bytes);
  try { return new TextDecoder("utf-8", { fatal: true }).decode(bytes); }
  catch { return new TextDecoder("gb18030").decode(bytes); }
}
export function enabledAxes(
  horizontal: boolean,
  vertical: boolean,
  changed: "horizontal" | "vertical",
): { horizontal: boolean; vertical: boolean } {
  return horizontal || vertical
    ? { horizontal, vertical }
    : changed === "horizontal"
      ? { horizontal: false, vertical: true }
      : { horizontal: true, vertical: false };
}
export function conversationTransform(face: boolean, flipped: boolean): string {
  return face && flipped ? "rotate(180deg)" : "none";
}
export function conversationOrder(
  face: boolean,
  swapped: boolean,
): ["A" | "B", "A" | "B"] {
  return face !== swapped ? ["B", "A"] : ["A", "B"];
}
export function setupVision(): void {
  const root = el("visionPanel");
  if (!root || root.dataset.ready) return;
  root.dataset.ready = "1";
  let revision = -1,
    imageRevision = -1,
    busy = false,
    format = "text",
    raw: unknown = {},
    stream: MediaStream | null = null,
    route = "visionTranslation",
    lastText = "",
    lastTranslation = "",
    pendingTranslation = false;
  let subtitleSegments: SubtitleSegment[] = [], advancedSrtDirty = false, subtitleServerIgnored = false;
  let cameraGeneration = 0,
    requestSerial = 0,
    latestAction = 0;
  const status = (s: string) => {
    el("visionStatus").textContent = s;
  };
  const send = (action: Action, extra: Record<string, unknown> = {}) => {
    if (["subtitles", "importSubtitles", "translateSubtitles"].includes(action)) subtitleServerIgnored = false;
    const sequence = ++requestSerial;
    if (action !== "status") latestAction = sequence;
    if (
      [
        "capture",
        "longCapture",
        "screen",
        "import",
        "recognize",
        "translate",
        "subtitles",
        "importSubtitles",
        "translateSubtitles",
      ].includes(action)
    )
      busy = true;
    window.chrome?.webview?.postMessage(
      serializeHostMessage({
        type: "visionRequest",
        data: {
          requestId: `vision-${action}-${sequence}`,
          action,
          revision,
          imageRevision,
          ...extra,
        },
      }),
    );
  };
  const download = (name: string, text: string, type = "text/plain") => {
    const url = URL.createObjectURL(new Blob([text], { type }));
    const a = document.createElement("a");
    a.href = url;
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 3000);
  };
  const copy = (text: string) => personalAction("copy", { text });
  for (const id of [
    "visionSource",
    "visionTarget",
    "conversationLangA",
    "conversationLangB",
  ]) {
    el<HTMLSelectElement>(id).replaceChildren(
      ...languages
        .filter(([key]) => id === "visionSource" || key !== "auto")
        .map(([key, label]) => new Option(label, key)),
    );
  }
  el<HTMLSelectElement>("visionSource").value = "auto";
  el<HTMLSelectElement>("visionTarget").value = "en";
  el<HTMLSelectElement>("conversationLangA").value = "zh";
  el<HTMLSelectElement>("conversationLangB").value = "en";
  const options = () => ({
    source: value("visionSource"),
    target: value("visionTarget"),
    task: value("visionTask"),
    horizontal: el<HTMLInputElement>("visionX").checked,
    vertical: el<HTMLInputElement>("visionY").checked,
  });
  for (const [id, action] of Object.entries({
    visionCapture: "capture",
    visionLong: "longCapture",
    visionFinish: "finish",
    visionStop: "stop",
    visionSaveImage: "saveImage",
    visionRecognize: "recognize",
    visionScreen: "screen",
  })) {
    el(id).onclick = () => {
      if (
        ["capture", "longCapture", "screen", "recognize"].includes(action) &&
        busy
      ) {
        status("请先停止当前任务");
        return;
      }
      if (action === "screen") route = "visionTranslation";
      send(action as Action, options());
    };
  }
  const translate = (
    text: string,
    source: string,
    target: string,
    destination: string,
  ) => {
    if (busy) {
      status("请等待当前任务完成");
      return;
    }
    if (!text.trim()) {
      status("请先填写原文");
      return;
    }
    route = destination;
    pendingTranslation = true;
    send("translate", { text, source, target });
  };
  el("visionTranslate").onclick = () =>
    translate(
      value("visionText"),
      value("visionSource"),
      value("visionTarget"),
      "visionTranslation",
    );
  el("conversationTranslateA").onclick = () =>
    translate(
      value("conversationTextA"),
      value("conversationLangA"),
      value("conversationLangB"),
      "conversationResultA",
    );
  el("conversationTranslateB").onclick = () =>
    translate(
      value("conversationTextB"),
      value("conversationLangB"),
      value("conversationLangA"),
      "conversationResultB",
    );
  let flipped = true,
    swapped = false;
  const layout = () => {
    const face = value("conversationLayout") === "face";
    el("conversationCards").classList.toggle("face", face);
    const order = conversationOrder(face, swapped);
    el("conversationCards").replaceChildren(
      ...order.map((x) => el("conversation" + x)),
    );
    const opposite = "conversation" + order[0];
    for (const id of ["conversationA", "conversationB"])
      el(id).style.transform =
        id === opposite ? conversationTransform(face, flipped) : "none";
    el<HTMLButtonElement>("conversationFlip").disabled = !face;
  };
  el("conversationLayout").onchange = layout;
  el("conversationFlip").onclick = () => {
    flipped = !flipped;
    layout();
  };
  el("conversationSwap").onclick = () => {
    swapped = !swapped;
    layout();
  };
  layout();
  el("visionCopy").onclick = () => copy(value("visionText"));
  el("visionCopyTranslation").onclick = () => copy(value("visionTranslation"));
  el("visionExport").onclick = () =>
    download(
      `知语识别.${format === "html" ? "html" : format === "latex" ? "tex" : "txt"}`,
      value("visionText"),
    );
  el("visionExportTranslation").onclick = () =>
    download("知语翻译.txt", value("visionTranslation"));
  el("visionRaw").onclick = () =>
    download(
      "知语识别原始结果.json",
      JSON.stringify(raw, null, 2),
      "application/json",
    );
  const importImage = (file: Blob) => {
    if (file.size > 25 * 1024 * 1024) {
      status("图片超过 25 MB，请缩小后导入");
      return;
    }
    if (busy) {
      status("请先停止当前任务");
      return;
    }
    const reader = new FileReader();
    reader.onload = () => send("import", { image: String(reader.result) });
    reader.onerror = () => status("读取图片失败");
    reader.readAsDataURL(file);
  };
  el<HTMLInputElement>("visionFile").onchange = () => {
    const f = el<HTMLInputElement>("visionFile").files?.[0];
    if (f) importImage(f);
    el<HTMLInputElement>("visionFile").value = "";
  };
  const stopCamera = () => {
    ++cameraGeneration;
    stream?.getTracks().forEach((t) => t.stop());
    stream = null;
    el<HTMLVideoElement>("visionVideo").srcObject = null;
    el("visionVideo").hidden = true;
    el<HTMLButtonElement>("visionShutter").disabled = true;
    el<HTMLButtonElement>("visionCameraStop").disabled = true;
    el<HTMLButtonElement>("visionCamera").disabled = false;
  };
  el("visionCamera").onclick = async () => {
    stopCamera();
    const generation = cameraGeneration;
    el<HTMLButtonElement>("visionCamera").disabled = true;
    try {
      const opened = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: { ideal: "environment" } },
        audio: false,
      });
      if (
        generation !== cameraGeneration ||
        document.hidden ||
        !root.getClientRects().length
      ) {
        opened.getTracks().forEach((t) => t.stop());
        return;
      }
      stream = opened;
      el<HTMLVideoElement>("visionVideo").srcObject = stream;
      el("visionVideo").hidden = false;
      el<HTMLButtonElement>("visionShutter").disabled = false;
      el<HTMLButtonElement>("visionCameraStop").disabled = false;
    } catch {
      status("无法打开摄像头，请允许权限，或使用“导入照片”");
    } finally {
      el<HTMLButtonElement>("visionCamera").disabled = false;
    }
  };
  el("visionCameraStop").onclick = stopCamera;
  el("visionShutter").onclick = () => {
    const video = el<HTMLVideoElement>("visionVideo");
    if (!video.videoWidth) {
      status("摄像头画面尚未就绪");
      return;
    }
    const canvas = document.createElement("canvas");
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    canvas.getContext("2d")!.drawImage(video, 0, 0);
    canvas.toBlob((blob) => {
      if (blob) importImage(blob);
    }, "image/png");
    stopCamera();
  };
  root
    .querySelectorAll<HTMLInputElement | HTMLSelectElement>("[data-vision]")
    .forEach(
      (input) =>
        (input.onchange = () => {
          const key = input.dataset.vision!;
          if (key === "capture_x" || key === "capture_y") {
            const axes = enabledAxes(
              el<HTMLInputElement>("visionX").checked,
              el<HTMLInputElement>("visionY").checked,
              key === "capture_x" ? "horizontal" : "vertical",
            );
            el<HTMLInputElement>("visionX").checked = axes.horizontal;
            el<HTMLInputElement>("visionY").checked = axes.vertical;
            for (const [name, on] of [
              ["capture_x", axes.horizontal],
              ["capture_y", axes.vertical],
            ] as const)
              if (on) updateConfig(`vision.${name}`, "true");
            for (const [name, on] of [
              ["capture_x", axes.horizontal],
              ["capture_y", axes.vertical],
            ] as const)
              if (!on) updateConfig(`vision.${name}`, "false");
          } else updateConfig(`vision.${key}`, input.value);
        }),
    );
  root.querySelectorAll<HTMLAnchorElement>("[data-open]").forEach(
    (a) =>
      (a.onclick = (e) => {
        e.preventDefault();
        window.chrome?.webview?.postMessage(
          serializeHostMessage({
            type: "openExternalUrl",
            data: a.dataset.open!,
          }),
        );
      }),
  );
  onHostMessage("configSnapshot", (message) => {
    const config = (message.data as Record<string, unknown>).vision as
      | Record<string, string>
      | undefined;
    if (!config) return;
    root
      .querySelectorAll<HTMLInputElement | HTMLSelectElement>("[data-vision]")
      .forEach((input) => {
        const v = config[input.dataset.vision!];
        if (v === undefined || document.activeElement === input) return;
        if (input instanceof HTMLInputElement && input.type === "checkbox")
          input.checked = v === "true";
        else input.value = v;
      });
  });
  const subtitleCards = new Map<string, HTMLElement>();
  let subtitleBackup: SubtitleDraft | null = null;
  let subtitlePreviousDraft: SubtitleDraft | null = null;
  const renderSubtitleCards = () => {
    const host = el("subtitleSegments");
    for (const segment of subtitleSegments) {
      let card = subtitleCards.get(segment.id);
      if (!card) {
        card = document.createElement("div");
        card.className = "subtitle-segment";
        const heading = document.createElement("strong");
        card.append(heading);
        for (const [field, title] of [["text", "原文"], ["translation", "译文"]] as const) {
          const label = document.createElement("label");
          label.textContent = title;
          const input = document.createElement("textarea");
          input.rows = 2;
          input.dataset.subtitleField = field;
          input.setAttribute("aria-label", `${title} ${segment.id}`);
          input.oninput = () => {
            const current = subtitleSegments.find((x) => x.id === segment.id);
            if (!current) return;
            current[field] = input.value;
            current[field === "text" ? "dirtyText" : "dirtyTranslation"] = true;
            if (!advancedSrtDirty) el<HTMLTextAreaElement>("subtitleSrt").value = renderSubtitleSrt(subtitleSegments);
          };
          label.append(input);
          card.append(label);
        }
        subtitleCards.set(segment.id, card);
        host.append(card);
      }
      card.querySelector("strong")!.textContent = `${subtitleTime(segment.start)} → ${subtitleTime(segment.end)}`;
      for (const input of card.querySelectorAll<HTMLTextAreaElement>("textarea")) {
        const field = input.dataset.subtitleField as "text" | "translation";
        if (input.value !== segment[field]) input.value = segment[field];
        input.disabled = advancedSrtDirty;
      }
    }
    el("subtitleEditStatus").textContent = advancedSrtDirty
      ? "高级 SRT 编辑受到保护：后续新段只会追加，不会重写已有内容。点击“解析编辑后的字幕”可继续分段编辑。"
      : `${subtitleSegments.length} 段字幕；分段的手动修改不会被新识别或翻译覆盖。`;
  };
  const resolveSubtitleFailure = (incoming?: unknown) => {
    if (!subtitleBackup) return;
    const previous = subtitleSegments;
    const resolved = recoverSubtitleFailure(subtitleSegments, incoming, subtitleBackup.segments);
    subtitleSegments = resolved.segments;
    const editor = el<HTMLTextAreaElement>("subtitleSrt");
    if (resolved.restored) {
      advancedSrtDirty = subtitleBackup.dirty;
      editor.value = subtitleBackup.editor;
      subtitleServerIgnored = true;
    } else editor.value = advancedSrtDirty ? appendFreshSubtitleCues(editor.value, previous, subtitleSegments) : renderSubtitleSrt(subtitleSegments);
    subtitleBackup = null;
    subtitleCards.clear();
    el("subtitleSegments").replaceChildren();
    renderSubtitleCards();
  };
  const rememberSubtitleDraft = () => {
    subtitleBackup = { segments: subtitleSegments.map((x) => ({ ...x })), editor: value("subtitleSrt"), dirty: advancedSrtDirty };
    subtitlePreviousDraft = subtitleBackup;
    el<HTMLButtonElement>("subtitlePreviousDraft").disabled = !subtitleBackup.editor.trim();
  };
  el("subtitlePreviousDraft").onclick = () => {
    if (busy) { status("请先结束当前任务"); return; }
    if (!subtitlePreviousDraft) return;
    const prior = subtitlePreviousDraft;
    subtitlePreviousDraft = { segments: subtitleSegments.map((x) => ({ ...x })), editor: value("subtitleSrt"), dirty: advancedSrtDirty };
    subtitleSegments = prior.segments.map((x) => ({ ...x }));
    advancedSrtDirty = prior.dirty;
    el<HTMLTextAreaElement>("subtitleSrt").value = prior.editor;
    subtitleServerIgnored = true;
    subtitleCards.clear();
    el("subtitleSegments").replaceChildren();
    renderSubtitleCards();
    status("已切换字幕草稿；再次点击可切回，随后新任务以当前稿为准");
  };
  const beginSubtitleDocument = (action: "subtitles" | "importSubtitles", extra: Record<string, unknown>) => {
    rememberSubtitleDraft();
    subtitleSegments = [];
    advancedSrtDirty = false;
    subtitleCards.clear();
    el("subtitleSegments").replaceChildren();
    send(action, extra);
  };
  el<HTMLTextAreaElement>("subtitleSrt").oninput = () => {
    advancedSrtDirty = true;
    renderSubtitleCards();
  };
  el<HTMLInputElement>("subtitleFile").onchange = async () => {
    const input = el<HTMLInputElement>("subtitleFile"), file = input.files?.[0];
    input.value = "";
    if (!file) return;
    if (busy) { status("请先结束当前任务"); return; }
    if (file.size > 1024 * 1024) { status("字幕超过 1 MB，请分段导入"); return; }
    try {
      const text = decodeSubtitleBytes(new Uint8Array(await file.arrayBuffer()));
      if (busy) { status("请先结束当前任务"); return; }
      beginSubtitleDocument("importSubtitles", { text });
    } catch { status("字幕文件读取失败，请使用 UTF-8、UTF-16 或 GB18030 编码"); }
  };
  el("subtitleParseEdited").onclick = () => {
    if (busy) { status("请先结束当前任务"); return; }
    beginSubtitleDocument("importSubtitles", { text: value("subtitleSrt") });
  };
  el("subtitleTranslateDocument").onclick = () => {
    if (busy) { status("请先结束当前任务"); return; }
    const text = advancedSrtDirty || !subtitleSegments.length ? value("subtitleSrt") : renderSubtitleSrt(subtitleSegments, false);
    if (!text.trim()) { status("请先导入字幕或采集声音"); return; }
    rememberSubtitleDraft();
    if (advancedSrtDirty) {
      // A changed time axis is re-parsed transactionally before translating.
      subtitleSegments = [];
      subtitleCards.clear();
      el("subtitleSegments").replaceChildren();
      advancedSrtDirty = false;
    } else {
      subtitleSegments = subtitleSegments.map((x, index) => ({ ...x, id: `file-${index + 1}` }));
      subtitleCards.clear();
      el("subtitleSegments").replaceChildren();
    }
    send("translateSubtitles", { text, source: value("visionSource"), target: value("visionTarget") });
  };
  onHostMessage("visionResponse", (message) => {
    if (Number(message.requestId.split("-").at(-1)) < latestAction) return;
    if (!message.ok) {
      busy = false;
      status(message.message);
      pendingTranslation = false;
      resolveSubtitleFailure(message.subtitles);
      return;
    }
    busy = Boolean(message.busy);
    status(message.error || message.message);
    if (typeof message.warning === "string") {
      el("visionWarning").textContent = message.warning;
      el("visionWarning").hidden = !message.warning;
    }
    if (typeof message.revision === "number") revision = message.revision;
    if (typeof message.text === "string" && message.text !== lastText) {
      lastText = message.text;
      el<HTMLTextAreaElement>("visionText").value = message.text;
    }
    if (
      typeof message.translation === "string" &&
      (message.translation !== lastTranslation || (pendingTranslation && !busy))
    ) {
      lastTranslation = message.translation;
      if (!busy || !pendingTranslation)
        el<HTMLTextAreaElement>(route).value = message.translation;
    }
    if (!busy) pendingTranslation = false;
    if (!busy && message.error) resolveSubtitleFailure(message.subtitles);
    else if (!busy) subtitleBackup = null;
    if (message.image) {
      el<HTMLImageElement>("visionImage").src = message.image;
      el("visionImage").hidden = false;
      el("visionDimensions").textContent =
        `${message.width} × ${message.height} 像素`;
    }
    if (message.raw) raw = message.raw;
    if (message.format) format = message.format;
  });
  onHostMessage("visionResponse", (message) => {
    if (Number(message.requestId.split("-").at(-1)) < latestAction) return;
    if (typeof message.imageRevision === "number")
      imageRevision = message.imageRevision;
    if (Array.isArray(message.subtitles) && !subtitleServerIgnored) {
      const previous = subtitleSegments;
      subtitleSegments = mergeSubtitleSegments(previous, message.subtitles);
      const editor = el<HTMLTextAreaElement>("subtitleSrt");
      editor.value = advancedSrtDirty ? appendFreshSubtitleCues(editor.value, previous, subtitleSegments) : renderSubtitleSrt(subtitleSegments);
      renderSubtitleCards();
    }
  });
  el("subtitleStart").onclick = () => {
    if (busy) {
      status("请先结束当前任务");
      return;
    }
    beginSubtitleDocument("subtitles", {
      ...options(),
      playback: value("subtitleSource") === "playback",
      translate: el<HTMLInputElement>("subtitleTranslate").checked,
    });
  };
  el("subtitleFinish").onclick = () => send("finish");
  el("subtitleStop").onclick = () => send("stop");
  el("subtitleExport").onclick = () =>
    download("知语字幕.srt", value("subtitleSrt"));
  el("subtitleCopy").onclick = () => copy(value("subtitleSrt"));
  window.addEventListener("msime-chat-hidden", () => {
    stopCamera();
    send("stop");
  });
  window.addEventListener("msime-vision-pane-change", (event) => {
    if ((event as CustomEvent<string>).detail !== "image") stopCamera();
  });
  const timer = window.setInterval(() => {
    if (busy || root.getClientRects().length) send("status");
  }, 700);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) stopCamera();
  });
  window.addEventListener("beforeunload", () => {
    stopCamera();
    send("stop");
    clearInterval(timer);
  });
  setupLocalOcr();
  window.chrome?.webview?.postMessage(
    serializeHostMessage({ type: "configRequest" }),
  );
  send("status");
}
