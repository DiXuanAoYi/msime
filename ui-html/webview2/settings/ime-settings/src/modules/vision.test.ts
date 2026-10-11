import { describe, it, expect, vi } from "vitest";
vi.mock("./config-sync", () => ({ updateConfig: vi.fn() }));
import {
  enabledAxes,
  conversationTransform,
  conversationOrder,
} from "./vision";
describe("vision interaction invariants", () => {
  it("always leaves a direction enabled, including switching the last enabled axis off", () => {
    expect(enabledAxes(false, false, "horizontal")).toEqual({
      horizontal: false,
      vertical: true,
    });
    expect(enabledAxes(false, false, "vertical")).toEqual({
      horizontal: true,
      vertical: false,
    });
    expect(enabledAxes(true, true, "horizontal")).toEqual({
      horizontal: true,
      vertical: true,
    });
    expect(enabledAxes(true, false, "horizontal")).toEqual({
      horizontal: true,
      vertical: false,
    });
  });
  it("rotates both axes together only in face-to-face mode", () => {
    expect(conversationTransform(true, true)).toBe("rotate(180deg)");
    expect(conversationTransform(true, false)).toBe("none");
    expect(conversationTransform(false, true)).toBe("none");
  });
  it("places the opposite participant at the top in face mode and swaps complete panels", () => {
    expect(conversationOrder(false, false)).toEqual(["A", "B"]);
    expect(conversationOrder(false, true)).toEqual(["B", "A"]);
    expect(conversationOrder(true, false)).toEqual(["B", "A"]);
    expect(conversationOrder(true, true)).toEqual(["A", "B"]);
  });
});
