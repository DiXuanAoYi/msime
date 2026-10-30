import { describe, it, expect, vi } from "vitest";
vi.mock("./config-sync", () => ({ updateConfig: vi.fn() }));
import {
  enabledAxes,
  conversationTransform,
  conversationOrder,
  mergeSubtitleSegments,
  appendFreshSubtitleCues,
  renderSubtitleSrt,
  decodeSubtitleBytes,
  recoverSubtitleFailure,
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
  it("keeps corrections and manually edited translations while appending new captured segments", () => {
    const original = [{ id: "capture-1", start: 0, end: 5, text: "人工修正", translation: "My correction", dirtyText: true, dirtyTranslation: true }];
    const next = mergeSubtitleSegments(original, [
      { id: "capture-1", start: 0, end: 5, text: "识别原文", translation: "Automatic result" },
      { id: "capture-2", start: 5, end: 10, text: "新的一段", translation: "Next segment" },
    ]);
    expect(next[0].text).toBe("人工修正");
    expect(next[0].translation).toBe("My correction");
    expect(next[1].text).toBe("新的一段");
    expect(original).toHaveLength(1);
    expect(mergeSubtitleSegments(next, [{ start: -1, end: 1, text: "bad" }])).toEqual(next);
  });
  it("does not apply translation of a stale source to a user-corrected segment", () => {
    const next = mergeSubtitleSegments([{ id: "a", start: 1, end: 2, text: "corrected", translation: "", dirtyText: true }],
      [{ id: "a", start: 1, end: 2, text: "old", translation: "wrong translation" }]);
    expect(next[0].translation).toBe("");
  });
  it("appends each new cue once without replacing advanced time-axis and code edits", () => {
    const first = [{ id: "a", start: 0, end: 5, text: "old", translation: "" }];
    const next = [...first, { id: "b", start: 5.125, end: 10, text: "next", translation: "下一段" }];
    const edited = "9\n00:00:01,000 --> 00:00:04,000\nunit_price = count * 2\n";
    const updated = appendFreshSubtitleCues(edited, first, next);
    expect(updated).toContain("00:00:01,000 --> 00:00:04,000\nunit_price = count * 2");
    expect(updated).toContain("2\n00:00:05,125 --> 00:00:10,000\nnext\n下一段");
    expect(appendFreshSubtitleCues(updated, next, next)).toBe(updated);
    const spaces = "1\n00:00:00,000 --> 00:00:05,000\nmanual_code   ";
    expect(appendFreshSubtitleCues(spaces, first, next)).toContain("manual_code   \n\n");
    expect(renderSubtitleSrt(next, false).includes("下一段")).toBe(false);
  });
  it("decodes Windows UTF-16 Chinese subtitle exports and standard UTF-8 without losing text", () => {
    expect(decodeSubtitleBytes(new Uint8Array([0xff, 0xfe, 0x60, 0x4f, 0x7d, 0x59]))).toBe("你好");
    expect(decodeSubtitleBytes(new TextEncoder().encode("WEBVTT\n\n你好"))).toBe("WEBVTT\n\n你好");
  });
  it("keeps completed translations after a later provider error and restores the old draft only when nothing new parsed", () => {
    const old = [{ id: "old", start: 0, end: 1, text: "old draft", translation: "" }];
    const partial = [{ id: "file-1", start: 4, end: 5, text: "new draft", translation: "已完成译文" }];
    expect(recoverSubtitleFailure([], partial, old)).toEqual({ segments: partial, restored: false });
    expect(recoverSubtitleFailure(partial, undefined, old)).toEqual({ segments: partial, restored: false });
    expect(recoverSubtitleFailure([], undefined, old)).toEqual({ segments: old, restored: true });
  });
});
