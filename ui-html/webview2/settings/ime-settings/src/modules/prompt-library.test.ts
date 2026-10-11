import { describe, it, expect, vi } from "vitest";
import { readFileSync } from "node:fs";
vi.mock("./config-sync", () => ({ updateConfig: vi.fn() }));
import { builtinTemplates, migrateBook, validateBook, mergeImportedBook } from "./prompt-library";
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
