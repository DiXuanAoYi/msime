import { describe, expect, it } from "vitest";
import { formatTranscript } from "./vision-conversation";

describe("formatTranscript", () => {
  it("lists each turn with who said it and the translation underneath", () => {
    expect(
      formatTranscript([
        { side: "A", source: "你好", translation: "Hello", from: "中文", to: "English" },
        { side: "B", source: "Nice to meet you", translation: "很高兴认识你", from: "English", to: "中文" },
      ]),
    ).toBe("我：你好\nHello\n\n对方：Nice to meet you\n很高兴认识你");
  });
});
