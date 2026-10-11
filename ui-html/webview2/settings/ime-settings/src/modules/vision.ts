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
  | "subtitles";
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
    lastSrt = "",
    pendingTranslation = false;
  let cameraGeneration = 0,
    requestSerial = 0,
    latestAction = 0;
  const status = (s: string) => {
    el("visionStatus").textContent = s;
  };
  const send = (action: Action, extra: Record<string, unknown> = {}) => {
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
      `水杉识别.${format === "html" ? "html" : format === "latex" ? "tex" : "txt"}`,
      value("visionText"),
    );
  el("visionExportTranslation").onclick = () =>
    download("水杉翻译.txt", value("visionTranslation"));
  el("visionRaw").onclick = () =>
    download(
      "水杉识别原始结果.json",
      JSON.stringify(raw, null, 2),
      "application/json",
    );
  const importImage = (file: Blob) => {
    if (file.size > 20 * 1024 * 1024) {
      status("图片超过 20 MB，请缩小后导入");
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
  onHostMessage("visionResponse", (message) => {
    if (Number(message.requestId.split("-").at(-1)) < latestAction) return;
    if (!message.ok) {
      busy = false;
      status(message.message);
      pendingTranslation = false;
      return;
    }
    busy = Boolean(message.busy);
    status(message.error || message.message);
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
    if (typeof message.srt === "string" && message.srt !== lastSrt) {
      lastSrt = message.srt;
      el<HTMLTextAreaElement>("subtitleSrt").value = message.srt;
    }
  });
  el("subtitleStart").onclick = () => {
    if (busy) {
      status("请先结束当前任务");
      return;
    }
    send("subtitles", {
      ...options(),
      playback: value("subtitleSource") === "playback",
      translate: el<HTMLInputElement>("subtitleTranslate").checked,
    });
  };
  el("subtitleFinish").onclick = () => send("finish");
  el("subtitleStop").onclick = () => send("stop");
  el("subtitleExport").onclick = () =>
    download("水杉字幕.srt", value("subtitleSrt"));
  el("subtitleCopy").onclick = () => copy(value("subtitleSrt"));
  window.addEventListener("msime-chat-hidden", () => {
    stopCamera();
    send("stop");
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
