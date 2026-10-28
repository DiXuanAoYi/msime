import type { ChipState, RawReader } from "./quick-controls";

/**
 * What the four quick pages need to say about "which model / which service is connected". Everything is read from the
 * existing config snapshot (voice_input, niutrans, tencent_tmt, vision), so the keys stay where the old pages left
 * them and no second copy of a secret is made.
 */

type Value = (path: string) => string | undefined;

const TEXT_MODEL_LABELS: Record<string, string> = {
  siliconflow: "硅基流动",
  openai: "OpenAI",
  deepseek: "DeepSeek",
  groq: "Groq",
  aliyun: "阿里云百炼",
  tencent: "腾讯 TokenHub",
};

const ASR_LABELS: Record<string, string> = {
  local: "本地离线",
  doubao: "豆包",
  openai: "OpenAI 兼容",
  siliconflow: "OpenAI 兼容",
  groq: "OpenAI 兼容",
  aliyun: "阿里云百炼",
  tencent: "腾讯云",
};

export const TEXT_PROVIDERS = ["deepseek", "siliconflow", "openai", "groq", "aliyun", "tencent"] as const;

/** The key stored for one text-model provider (the legacy single token only counts for the selected one). */
export function textProviderKey(value: Value, raw: RawReader, provider: string): string {
  return tokenOf(raw, "polish_token", provider, (value("voice_input.polish_provider") || "siliconflow").toLowerCase());
}

export function asrProviderKey(value: Value, raw: RawReader, provider: string): string {
  return tokenOf(raw, "asr_token", provider, (value("voice_input.asr_provider") || "doubao").toLowerCase());
}

export function textModelLabel(provider: string): string {
  return TEXT_MODEL_LABELS[provider.toLowerCase()] ?? (provider || "未选择");
}

export function asrLabel(provider: string): string {
  return ASR_LABELS[provider.toLowerCase()] ?? (provider || "未选择");
}

function tokenOf(raw: RawReader, field: string, provider: string, current = ""): string {
  const map = raw(`voice_input.${field}s`);
  if (map && typeof map === "object") {
    const hit = (map as Record<string, unknown>)[provider];
    if (typeof hit === "string" && hit.trim()) return hit.trim();
  }
  // The single legacy token belongs to whichever provider is selected; it says nothing about the others.
  const single = provider === current ? raw(`voice_input.${field}`) : undefined;
  return typeof single === "string" ? single.trim() : "";
}

/** The text model a feature uses: its own override, else the one chosen for speech polishing. */
export function textModel(value: Value, raw: RawReader, override = "") {
  const provider = (override || value("voice_input.polish_provider") || "siliconflow").toLowerCase();
  const same = provider === (value("voice_input.polish_provider") || "siliconflow").toLowerCase();
  const model = same ? value("voice_input.polish_model") || "" : "";
  const token = tokenOf(raw, "polish_token", provider, (value("voice_input.polish_provider") || "siliconflow").toLowerCase());
  return { provider, label: textModelLabel(provider), model, connected: token !== "" };
}

export function textModelChip(value: Value, raw: RawReader, override = ""): ChipState {
  const model = textModel(value, raw, override);
  return model.connected
    ? { text: `${model.label} 已连接`, tone: "good" }
    : { text: `${model.label} 未连接`, tone: "off" };
}

export function asrConnected(value: Value, raw: RawReader): boolean {
  const provider = (value("voice_input.asr_provider") || "doubao").toLowerCase();
  if (provider === "local") return true;
  if (provider === "tencent") return !!value("voice_input.tencent_secret_id");
  return tokenOf(raw, "asr_token", provider, provider) !== "" || !!value("voice_input.asr_app_key");
}

export function voiceChips(value: Value, raw: RawReader): { asr: ChipState; polish: ChipState } {
  const provider = (value("voice_input.asr_provider") || "doubao").toLowerCase();
  const asr: ChipState = {
    text: `识别：${asrLabel(provider)}${asrConnected(value, raw) ? "" : "（未连接）"}`,
    tone: asrConnected(value, raw) ? "good" : "warn",
  };
  const model = textModel(value, raw);
  const polish: ChipState =
    value("voice_input.polish_text") === "true"
      ? { text: `润色：${model.label}${model.connected ? "" : "（未连接）"}`, tone: model.connected ? "good" : "warn" }
      : { text: "润色：关", tone: "off" };
  return { asr, polish };
}

/** Per-engine state for the 识别引擎 cards: is a key stored for that provider? */
export function asrCardChip(card: string, value: Value, raw: RawReader): ChipState {
  if (card === "local") return { text: "已就绪", tone: "good" };
  const providers = card === "openai" ? ["openai", "siliconflow", "groq"] : [card];
  const connected = providers.some(
    (provider) =>
      tokenOf(raw, "asr_token", provider, (value("voice_input.asr_provider") || "doubao").toLowerCase()) !== "" ||
      (card === "tencent" && !!value("voice_input.tencent_secret_id")) ||
      (provider === "doubao" && !!value("voice_input.asr_app_key")),
  );
  return connected ? { text: "已连接", tone: "good" } : { text: "未连接", tone: "off" };
}

export function niuConnected(value: Value): boolean {
  return value("niutrans.enabled") === "true" && !!value("niutrans.apikey");
}

export function tencentTmtConnected(value: Value): boolean {
  return !!value("tencent_tmt.secret_id") && !!value("tencent_tmt.secret_key");
}

export function cloudOcrConnected(value: Value): boolean {
  return (
    !!value("vision.shiliu_appcode") ||
    (!!value("vision.tencent_secret_id") && !!value("vision.tencent_secret_key")) ||
    (!!value("vision.baidu_api_key") && !!value("vision.baidu_secret_key"))
  );
}
