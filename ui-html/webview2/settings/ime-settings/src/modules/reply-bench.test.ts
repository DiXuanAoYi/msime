import { describe, expect, it } from "vitest";
import { parseChat } from "./reply-bench";

describe("parseChat", () => {
  it("splits plain 名字：内容 lines and marks my own lines", () => {
    expect(parseChat("林经理：明天几点开会？\n我：上午十点")).toEqual([
      { who: "other", name: "林经理", text: "明天几点开会？" },
      { who: "me", name: "我", text: "上午十点" },
    ]);
  });
  it("reads the appended [时间] 名字：\n内容 blocks and multi-line messages", () => {
    const rows = parseChat("[时间未提供] 小林：\n第一行\n第二行\n\n[2026-10-28 09:00] 我：\n好的");
    expect(rows[0]).toMatchObject({ who: "other", name: "小林", text: "第一行\n第二行" });
    expect(rows[1]).toMatchObject({ who: "me", text: "好的" });
  });
  it("keeps text without speakers as a note", () => {
    expect(parseChat("随手记一下")).toEqual([{ who: "note", name: "", text: "随手记一下" }]);
  });
});
