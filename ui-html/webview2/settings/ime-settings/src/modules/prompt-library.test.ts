import { describe, it, expect, vi } from "vitest";
import { readFileSync } from "node:fs";
vi.mock("./config-sync", () => ({ updateConfig: vi.fn() }));
import { builtinTemplates, migrateBook, validateBook, mergeImportedBook, normalizePreservedNames } from "./prompt-library";
describe("prompt libraries", () => {
  it("merges imports without replacing existing templates or selections",()=>{
    const current=validateBook({version:1,selected:"same",items:[{id:"same",name:"我的模板",prompt:"原内容"}]});
    const merged=mergeImportedBook("association",current,{version:1,scope:"association",selected:"same",items:[{id:"same",name:"导入模板",prompt:"新内容"},{id:"copy",name:"我的模板",prompt:"原内容"}]});
    expect(merged.items).toHaveLength(2);expect(merged.selected).toBe("same");expect(merged.items[0]).toEqual(current.items[0]);expect(merged.items[1].id).not.toBe("same");expect(current.items).toHaveLength(1);
    expect(()=>mergeImportedBook("polish",current,{...merged,scope:"association"})).toThrow();
    const full=validateBook({version:1,selected:"x0",items:Array.from({length:128},(_,i)=>({id:`x${i}`,name:`模板${i}`,prompt:"内容"}))});
    expect(()=>mergeImportedBook("association",full,current)).toThrow();expect(full.items).toHaveLength(128);
  });
  it("mounts the library in both real settings pages and removes legacy fixed slots", () => {
    for (const [file, id] of [
      ["voice.html", "voicePromptLibrary"],
      ["ai-settings.html", "aiPromptLibrary"],
    ]) {
      const html = readFileSync(
        new URL("../partials/" + file, import.meta.url),
        "utf8",
      );
      expect(html).toContain(`id="${id}"`);
      expect(html).not.toMatch(/data-value="custom_[123]"/);
    }
  });
  it("preserves all legacy contents and the active slot with useful names", () => {
    const book = migrateBook("association", {
      prompt_id: "custom_2",
      prompt: "第二份原文",
      prompt_custom_1: "第一份原文",
      prompt_custom_2: "第二份原文",
      prompt_custom_3: "第三份原文",
    });
    for (const text of ["第一份原文", "第二份原文", "第三份原文"])
      expect(book.items.some((x) => x.prompt === text)).toBe(true);
    expect(book.items.find((x) => x.id === book.selected)?.prompt).toBe(
      "第二份原文",
    );
    expect(book.items.some((x) => /^自定义[一二三]$/.test(x.name))).toBe(false);
  });
  it("uses short functional legacy names and only repairs its own previous automatic names", () => {
    const full = "你是中文全拼输入法联想引擎。输入为已经切分好的拼音数组、前文上下文。";
    const double = "你是中文双拼输入法联想引擎。保留原提示词的完整正文。";
    const config = { prompt_custom_1: full, prompt_custom_2: double, prompt_custom_3: "保留第三份原文" };
    const migrated = migrateBook("association", config);
    expect(migrated.items.find(x => x.id === "preserved-1")?.name).toBe("原有中文拼音联想（A）");
    expect(migrated.items.find(x => x.id === "preserved-2")?.name).toBe("原有双拼联想（B）");
    expect(migrated.items.filter(x => x.id.startsWith("preserved-")).every(x => [...x.name].length <= 16)).toBe(true);
    const oldName = "原有 · " + full.replace(/^[你您]是(?:一个)?/, "").slice(0, 30);
    const previous = validateBook({ version: 1, selected: "preserved-1", items: [
      { id: "preserved-1", name: oldName, prompt: full },
      { id: "preserved-2", name: "我的工作联想提示词，名称保留", prompt: double },
      { id: "custom-abc", name: "原有 · 我自己添加的模板", prompt: "用户正文" },
    ] });
    const repaired = normalizePreservedNames("association", previous, config);
    expect(repaired.items[0]).toEqual({ id: "preserved-1", name: "原有中文拼音联想（A）", prompt: full });
    expect(repaired.items.slice(1)).toEqual(previous.items.slice(1));
    expect(previous.items[0].name).toBe(oldName);
    expect(repaired.selected).toBe(previous.selected);
  });
  it("preserves custom polishing and selected built-in text on migration", () => {
    const book = migrateBook(
      "polish",
      {
        polish_prompt_id: "custom_2",
        polish_prompt: "",
        polish_prompt_custom_2: "保留这份原文",
      },
      "默认规则",
    );
    expect(book.items.find((x) => x.id === book.selected)?.prompt).toBe(
      "保留这份原文",
    );
    expect(
      migrateBook(
        "polish",
        { polish_prompt_id: "faithful" },
        "原有忠实校对",
      ).items.some((x) => x.prompt === "原有忠实校对"),
    ).toBe(true);
  });
  it("rejects duplicate identities, unknown selections and oversized imported content", () => {
    const x = { id: "one", name: "模板", prompt: "内容" };
    expect(() =>
      validateBook({ version: 1, selected: "one", items: [x, x] }),
    ).toThrow();
    expect(() =>
      validateBook({ version: 1, selected: "missing", items: [x] }),
    ).toThrow();
    expect(() =>
      validateBook({
        version: 1,
        selected: "one",
        items: [{ ...x, prompt: "中".repeat(6000) }],
      }),
    ).toThrow();
  });
  it("offers substantial preset coverage and keeps the AI JSON contract", () => {
    expect(builtinTemplates("polish").length).toBeGreaterThanOrEqual(30);
    expect(builtinTemplates("association").length).toBeGreaterThanOrEqual(24);
    for (const x of builtinTemplates("association"))
      expect(x.prompt).toContain('"candidates"');
  });
});
