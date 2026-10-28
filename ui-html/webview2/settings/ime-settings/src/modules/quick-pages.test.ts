import { expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { comboFromEvent, comboParts, configValue } from "./quick-controls";
import { addWord, moveTone, newToneId, personaPreview, type Persona, type Tone } from "./reply-quick";
import { filterHistory, historyAge, historyKindLabel, type VisionHistoryRow } from "./vision-quick";

const key = (init: Partial<KeyboardEvent> & { code: string; key: string }) => init as KeyboardEvent;
const partial = (name: string) => readFileSync(new URL(`../partials/${name}.html`, import.meta.url), "utf8");

it("turns a key press into the text form the Server accepts", () => {
  expect(comboFromEvent(key({ code: "KeyX", key: "x", ctrlKey: true, altKey: true }))).toBe("Ctrl+Alt+X");
  expect(comboFromEvent(key({ code: "KeyT", key: "T", ctrlKey: true, altKey: true, shiftKey: true }))).toBe("Ctrl+Alt+Shift+T");
  expect(comboFromEvent(key({ code: "F9", key: "F9", ctrlKey: true }))).toBe("Ctrl+F9");
  expect(comboFromEvent(key({ code: "Digit1", key: "1", altKey: true }))).toBe("Alt+1");
});

it("ignores modifier-only presses and keys without Ctrl, Alt or Win", () => {
  expect(comboFromEvent(key({ code: "ControlLeft", key: "Control", ctrlKey: true }))).toBeNull();
  expect(comboFromEvent(key({ code: "KeyX", key: "x" }))).toBeNull();
  expect(comboFromEvent(key({ code: "KeyX", key: "x", shiftKey: true }))).toBeNull();
});

it("splits a combo for display and reads config values by path", () => {
  expect(comboParts("Ctrl+Alt+X")).toEqual(["Ctrl", "Alt", "X"]);
  expect(comboParts("")).toEqual([]);
  expect(configValue({ reply: { count: "3" } }, "reply.count")).toBe("3");
  expect(configValue({}, "reply.count")).toBeUndefined();
});

const persona = (over: Partial<Persona> = {}): Persona => ({
  fillers: [], banned: [], emoji: "none", punct: "normal", address: "contact", about: "", ...over,
});

it("previews the persona on a sample reply", () => {
  const sample = "好的，我明天把方案发给您。请随时告诉我。";
  expect(personaPreview(sample, persona())).toBe(sample);
  expect(personaPreview(sample, persona({ address: "ni" }))).toContain("发给你");
  expect(personaPreview(sample, persona({ fillers: ["嗯嗯"] }))).toMatch(/^嗯嗯，/);
  expect(personaPreview(sample, persona({ banned: ["请"] }))).not.toContain("请");
  expect(personaPreview(sample, persona({ punct: "less" }))).not.toContain("。");
  expect(personaPreview(sample, persona({ punct: "none" }))).not.toMatch(/[，。]/);
  expect(personaPreview(sample, persona({ emoji: "many" }))).toContain("🙂👍");
});

const tone = (id: string): Tone => ({ id, name: id, desc: "", example: "", on: true, custom: false });

it("reorders tones without mutating the input and ignores moves past the ends", () => {
  const list = [tone("a"), tone("b"), tone("c")];
  expect(moveTone(list, 1, -1).map((t) => t.id)).toEqual(["b", "a", "c"]);
  expect(moveTone(list, 0, -1)).toBe(list);
  expect(moveTone(list, 2, 1)).toBe(list);
  expect(list.map((t) => t.id)).toEqual(["a", "b", "c"]);
});

it("makes unique safe ids for custom tones and limits word chips", () => {
  const first = newToneId([], 1000);
  expect(first).toMatch(/^[A-Za-z0-9_-]{1,40}$/);
  expect(newToneId([tone(first)], 1000)).not.toBe(first);
  expect(addWord(["嗯"], " 哈哈 ", 10)).toEqual(["嗯", "哈哈"]);
  expect(addWord(["嗯"], "嗯", 10)).toEqual(["嗯"]);
  expect(addWord(["嗯"], "   ", 10)).toEqual(["嗯"]);
  expect(addWord(["嗯"], "哈", 1)).toEqual(["嗯"]);
});

const row = (over: Partial<VisionHistoryRow>): VisionHistoryRow => ({
  id: "1", at: 0, kind: "ocr", text: "", translation: "", ...over,
});

it("filters history by kind and by text or translation", () => {
  const rows = [
    row({ id: "1", kind: "ocr", text: "Hello world" }),
    row({ id: "2", kind: "translate", text: "price", translation: "价格" }),
    row({ id: "3", kind: "selection", text: "cat", translation: "猫" }),
  ];
  expect(filterHistory(rows, "all", "").length).toBe(3);
  expect(filterHistory(rows, "translate", "").map((r) => r.id)).toEqual(["2"]);
  expect(filterHistory(rows, "all", "价").map((r) => r.id)).toEqual(["2"]);
  expect(filterHistory(rows, "all", "HELLO").map((r) => r.id)).toEqual(["1"]);
});

it("describes how long ago a record was made", () => {
  const now = Date.UTC(2026, 9, 15, 12, 0, 0);
  expect(historyAge(now - 10_000, now)).toBe("刚刚");
  expect(historyAge(now - 5 * 60_000, now)).toBe("5 分钟前");
  expect(historyAge(now - 3 * 3_600_000, now)).toBe("3 小时前");
  expect(historyAge(0, now)).toBe("");
  expect(historyKindLabel("selection")).toBe("划词翻译");
  expect(historyKindLabel("other")).toBe("记录");
});

it("binds every config path used by the new pages to a key the Server knows", () => {
  const known: Record<string, string[]> = {
    vision: ["provider", "after_ocr_copy", "after_ocr_card", "line_mode", "cjk_space", "translation_provider",
      "text_provider", "auto_swap", "zh_target", "source_language", "target_language", "live_refresh",
      "history_enabled", "history_days", "enter_action", "magnifier", "hide_toolbar", "table_format", "overlay_style",
      "hold_alt_original", "skip_code", "use_glossary"],
    reply: ["count", "length", "source", "context", "fill_mode", "provider", "reader_enabled"],
    clipboard: ["sort", "max_items", "retention_days", "respect_privacy_flag", "exclude_apps", "pause_on_lock"],
    hotkeys: ["enabled"],
  };
  for (const name of ["vision", "reply-studio", "personal-library"]) {
    const html = partial(name);
    for (const [, path] of html.matchAll(/data-cfg="([a-z_]+\.[a-z_]+)"/g)) {
      const [section, k] = path.split(".");
      expect(known[section], `${name}: ${path}`).toContain(k);
    }
  }
});

it("keeps ids unique and the old workbench controls inside the new pages", () => {
  const mustKeep: Record<string, string[]> = {
    vision: ["visionSettings", "visionHistGrid", "visionLinePreview"],
    "vision-bench": ["visionPanel", "visionCapture", "visionText", "visionBench"],
    "reply-studio": ["replyStudio", "rsGenerate", "rsReplies", "replyBench"],
    "personal-library": ["personalLibraryPanel", "libraryRows", "librarySearch", "libraryContent"],
  };
  for (const [name, ids] of Object.entries(mustKeep)) {
    const html = partial(name);
    const found = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
    expect(new Set(found).size, `${name} has duplicate ids`).toBe(found.length);
    for (const id of ids) expect(found, `${name} lost #${id}`).toContain(id);
    expect(html.split("<div").length - 1, `${name} unbalanced`).toBe(html.split("</div>").length - 1);
  }
});
