import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";

/**
 * The 语音输入 tab's try-it area and engine cards. The microphone button reuses the voice workbench's recorder and
 * local recognizer (voiceStudioRequest), so it tests recording and local recognition for real; it never types into
 * another app. The engine cards only drive the existing 服务提供商 menu, so the old fields keep working.
 */

type StudioState = { phase?: string; transcript?: string; hasInput?: boolean; inputSeconds?: number };

const CARD_GROUPS: Record<string, string[]> = {
  local: ["local"],
  doubao: ["doubao"],
  openai: ["openai", "siliconflow", "groq"],
  aliyun: ["aliyun"],
  tencent: ["tencent"],
};

export function asrCardForProvider(provider: string): string {
  return Object.entries(CARD_GROUPS).find(([, ids]) => ids.includes(provider))?.[0] ?? "";
}

export function setupVoiceHero(): void {
  const root = document.getElementById("voiceHero");
  if (!root || root.dataset.ready) return;
  root.dataset.ready = "1";
  const $ = <T extends HTMLElement>(id: string) => root.querySelector<T>(`#${id}`)!;
  const mic = $("micBtn");
  const label = $("micLabel");
  const out = $("vt");
  const pipe = $("pipe");
  let serial = 0;
  let holding = false;
  let step: "idle" | "recording" | "finishing" | "recognizing" = "idle";
  let timer = 0;

  const post = (action: string) =>
    window.chrome?.webview?.postMessage(
      serializeHostMessage({ type: "voiceStudioRequest", data: { requestId: `hero-${++serial}`, action: action as "status" } }),
    );

  const paintPipe = () => {
    const steps = [
      ["录音", step === "recording"],
      ["本机识别", step === "recognizing"],
      ["润色", false],
      ["上屏", false],
    ] as const;
    pipe.replaceChildren();
    steps.forEach(([name, on], index) => {
      if (index) {
        const arrow = document.createElement("em");
        arrow.textContent = "→";
        pipe.append(arrow);
      }
      const chip = document.createElement("span");
      chip.textContent = name;
      if (on) chip.className = "on";
      if (index >= 2) chip.className = "offline";
      pipe.append(chip);
    });
    const note = document.createElement("em");
    note.textContent = "测试只走录音和本机识别，不润色、不上屏";
    pipe.append(note);
  };

  const show = (text: string, kind: "hint" | "live" | "final" | "error" = "hint") => {
    out.replaceChildren();
    const line = document.createElement("span");
    line.className = kind === "hint" ? "vt-ph" : kind === "error" ? "vt-err" : kind === "live" ? "vt-live" : "";
    line.textContent = text;
    out.append(line);
  };

  const stopPolling = () => window.clearInterval(timer);
  const startPolling = () => {
    stopPolling();
    timer = window.setInterval(() => post("status"), 400);
  };

  const begin = (event: Event) => {
    event.preventDefault();
    if (holding || step !== "idle") return;
    holding = true;
    step = "recording";
    mic.classList.add("rec");
    label.textContent = "松开结束";
    show("正在听…", "live");
    paintPipe();
    post("recordStart");
    startPolling();
  };
  const end = () => {
    if (!holding) return;
    holding = false;
    mic.classList.remove("rec");
    label.textContent = "处理中…";
    step = "finishing";
    post("recordStop");
  };
  mic.addEventListener("pointerdown", begin);
  mic.addEventListener("pointerup", end);
  mic.addEventListener("pointerleave", end);
  mic.addEventListener("pointercancel", end);

  onHostMessage("voiceStudioResponse", (message) => {
    if (message.requestId && !String(message.requestId).startsWith("hero-")) return;
    const state = (message.state ?? {}) as StudioState;
    if (!message.ok && step !== "idle") {
      step = "idle";
      stopPolling();
      label.innerHTML = "按住说话<br>或按住右 Alt";
      show(message.message || "没有录到声音，请检查麦克风。", "error");
      paintPipe();
      return;
    }
    if (step === "finishing" && state.phase !== "recording") {
      if (state.hasInput) {
        step = "recognizing";
        show("正在识别…", "live");
        paintPipe();
        post("transcribe");
      } else {
        step = "idle";
        stopPolling();
        label.innerHTML = "按住说话<br>或按住右 Alt";
        show("没有录到声音，请检查麦克风。", "error");
        paintPipe();
      }
      return;
    }
    if (step === "recognizing" && state.phase !== "working") {
      step = "idle";
      stopPolling();
      label.innerHTML = "按住说话<br>或按住右 Alt";
      const text = String(state.transcript ?? "").trim();
      show(text || "没有识别出文字。", text ? "final" : "error");
      paintPipe();
    }
  });

  // Engine cards: drive the existing provider menu, then repaint from the snapshot.
  const cards = [...root.parentElement!.querySelectorAll<HTMLElement>(".eng[data-asr]")];
  cards.forEach((card) =>
    card.addEventListener("click", () => {
      const id = CARD_GROUPS[card.dataset.asr!]?.[0];
      document.querySelector<HTMLElement>(`#voiceAsrProviderMenu .dropdown-item[data-value="${id}"]`)?.click();
    }),
  );
  onHostMessage("configSnapshot", (message) => {
    const provider = String((message.data as Record<string, Record<string, unknown>>)?.voice_input?.asr_provider ?? "");
    const active = asrCardForProvider(provider);
    cards.forEach((card) => card.setAttribute("aria-pressed", String(card.dataset.asr === active)));
  });
  paintPipe();
}
