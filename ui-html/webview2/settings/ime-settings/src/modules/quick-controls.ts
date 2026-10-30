import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import { updateConfig } from "./config-sync";
import { openLibraryView, openPage } from "./page-links";
import "../styles/modules/quick-pages.css";

/**
 * Controls shared by the screenshot, reply, clipboard and voice pages. A control opts in with
 * data-cfg="section.key"; the page needs no per-control code. Everything is written through the same
 * configUpdate path as the rest of the settings, so a failed write leaves the old value on screen.
 */

type Snapshot = Record<string, Record<string, unknown> | undefined>;

export type ChipTone = "good" | "warn" | "bad" | "off" | "accent" | "plain";
export interface ChipState {
  text: string;
  tone: ChipTone;
}
/** Reads a raw config value (objects included), e.g. the per-provider token maps. */
export type RawReader = (path: string) => unknown;
/** Resolves a status chip (data-chip="name") from the latest config values. */
export type ChipResolver = (value: (path: string) => string | undefined, raw: RawReader) => ChipState | null;
/** Adds computed flags (data-show-if="vision.xyz") on top of the real config values. */
export type Deriver = (value: (path: string) => string | undefined, raw: RawReader) => Record<string, string>;

let counter = 0;
const nextId = (prefix: string) => `${prefix}-${Date.now().toString(36)}-${++counter}`;

export function rawValue(snapshot: Snapshot, path: string): unknown {
  const [section, key] = path.split(".");
  return snapshot[section]?.[key];
}

export function configValue(snapshot: Snapshot, path: string): string | undefined {
  const value = rawValue(snapshot, path);
  if (value === undefined || value === null || typeof value === "object") return undefined;
  return String(value);
}

/** "Ctrl+Alt+X" -> ["Ctrl", "Alt", "X"]; empty text means the action has no key. */
export function comboParts(text: string): string[] {
  return text ? text.split("+") : [];
}

const MODIFIER_KEYS = new Set(["Control", "Alt", "Shift", "Meta", "AltGraph"]);

/** The text form the Server accepts, or null while only modifiers (or a bare key) are held. */
export function comboFromEvent(event: KeyboardEvent): string | null {
  if (MODIFIER_KEYS.has(event.key)) return null;
  if (!event.ctrlKey && !event.altKey && !event.metaKey) return null;
  let key = "";
  if (/^Key[A-Z]$/.test(event.code)) key = event.code.slice(3);
  else if (/^Digit[0-9]$/.test(event.code)) key = event.code.slice(5);
  else if (/^F([1-9]|1[0-9]|2[0-4])$/.test(event.code)) key = event.code;
  else {
    const named: Record<string, string> = {
      Space: "Space", Enter: "Enter", Tab: "Tab", Backspace: "Backspace", Delete: "Delete", Insert: "Insert",
      Home: "Home", End: "End", PageUp: "PageUp", PageDown: "PageDown", ArrowUp: "Up", ArrowDown: "Down",
      ArrowLeft: "Left", ArrowRight: "Right", Escape: "Esc",
    };
    key = named[event.code] ?? "";
  }
  if (!key) return null;
  const parts: string[] = [];
  if (event.ctrlKey) parts.push("Ctrl");
  if (event.altKey) parts.push("Alt");
  if (event.shiftKey) parts.push("Shift");
  if (event.metaKey) parts.push("Win");
  parts.push(key);
  return parts.join("+");
}

function renderCombo(button: HTMLElement, text: string): void {
  button.replaceChildren();
  const parts = comboParts(text);
  if (!parts.length) {
    button.textContent = "未设置";
    return;
  }
  const wrap = document.createElement("span");
  wrap.className = "keys";
  parts.forEach((part, index) => {
    if (index) {
      const plus = document.createElement("i");
      plus.textContent = "+";
      wrap.append(plus);
    }
    const key = document.createElement("kbd");
    key.textContent = part;
    wrap.append(key);
  });
  button.append(wrap);
}

export function setSeg(root: HTMLElement, name: string, tab: string): void {
  root.querySelectorAll<HTMLElement>(`.seg[data-seg="${name}"] button`).forEach((button) => {
    button.setAttribute("aria-selected", String(button.dataset.tab === tab));
  });
  root.querySelectorAll<HTMLElement>(`.tabpane[data-pane-of="${name}"]`).forEach((pane) => {
    pane.hidden = pane.dataset.pane !== tab;
  });
  root.dispatchEvent(new CustomEvent("qp-tab", { detail: { name, tab } }));
}

export function postQuickAction(action: "run" | "status", id?: string): void {
  window.chrome?.webview?.postMessage(
    serializeHostMessage({
      type: "quickActionRequest",
      data: { requestId: nextId("quick"), action, ...(id ? { id } : {}) },
    }),
  );
}

export interface QuickControls {
  /** Latest config values, so page code can read what the controls show. */
  value(path: string): string | undefined;
  /** Raw config value (objects included). */
  raw(path: string): unknown;
  /** Re-applies the last snapshot (after the page rendered more controls). */
  refresh(): void;
}

export function bindQuickControls(
  root: HTMLElement,
  onChange?: (path: string, value: string) => void,
  chips: Record<string, ChipResolver> = {},
  derive?: Deriver,
): QuickControls {
  let snapshot: Snapshot = {};
  let derived: Record<string, string> = {};
  const read = (path: string) => derived[path] ?? configValue(snapshot, path);
  const raw: RawReader = (path) => rawValue(snapshot, path);
  let hotkeyState: Record<string, { combo?: string; ok?: boolean; error?: string }> = {};
  const hotkeyErrors = new Map<string, HTMLElement>();

  const applyVisibility = () => {
    root.querySelectorAll<HTMLElement>("[data-show-if]").forEach((node) => {
      const raw = node.dataset.showIf ?? "";
      const negate = raw.startsWith("!");
      const path = negate ? raw.slice(1) : raw;
      const value = read(path) === "true";
      node.hidden = negate ? value : !value;
    });
  };

  const apply = () => {
    derived = derive ? derive((path) => configValue(snapshot, path), raw) : {};
    root.querySelectorAll<HTMLElement>("[data-cfg]").forEach((node) => {
      const path = node.dataset.cfg!;
      const value = configValue(snapshot, path);
      if (value === undefined) return;
      if (node.classList.contains("tg")) node.setAttribute("aria-checked", String(value === "true"));
      else if (node instanceof HTMLSelectElement || node instanceof HTMLInputElement || node instanceof HTMLTextAreaElement) {
        if (document.activeElement !== node) node.value = value;
      } else {
        node.querySelectorAll<HTMLElement>("[data-v]").forEach((button) => {
          const any = button.dataset.any?.split(" ") ?? [];
          button.setAttribute("aria-pressed", String(button.dataset.v === value || any.includes(value)));
        });
      }
    });
    root.querySelectorAll<HTMLElement>("button.hk[data-hk]").forEach((button) => {
      if (button.classList.contains("capturing")) return;
      renderCombo(button, String(snapshot.hotkeys?.[button.dataset.hk!] ?? ""));
    });
    applyVisibility();
    paintChips();
    paintHotkeyStatus();
  };

  const paintChips = () => {
    root.querySelectorAll<HTMLElement>("[data-chip]").forEach((node) => {
      const resolve = chips[node.dataset.chip!];
      const state = resolve?.(read, raw);
      node.hidden = !state;
      if (!state) return;
      node.className = `chip ${state.tone}`;
      node.textContent = state.text;
    });
  };

  const paintHotkeyStatus = () => {
    root.querySelectorAll<HTMLElement>("button.hk[data-hk]").forEach((button) => {
      const action = button.dataset.hk!;
      const state = hotkeyState[action];
      let note = hotkeyErrors.get(action);
      const enabled = snapshot.hotkeys?.enabled !== "false";
      const failed = !!state && !state.ok && !!state.combo && enabled && state.error !== "未设置";
      button.classList.toggle("bad", failed);
      if (!failed) {
        note?.remove();
        hotkeyErrors.delete(action);
        return;
      }
      if (!note) {
        note = document.createElement("span");
        note.className = "chip bad hk-err";
        button.after(note);
        hotkeyErrors.set(action, note);
      }
      note.textContent = state.error || "快捷键没有生效";
    });
  };

  const write = (path: string, value: string) => {
    const [section, key] = path.split(".");
    snapshot = { ...snapshot, [section]: { ...(snapshot[section] ?? {}), [key]: value } };
    updateConfig(path, value);
    apply();
    onChange?.(path, value);
    if (section === "hotkeys") window.setTimeout(() => postQuickAction("status"), 1600);
  };

  root.addEventListener("click", (event) => {
    const target = event.target as HTMLElement;
    const toggle = target.closest<HTMLElement>("button.tg[data-cfg]");
    if (toggle && root.contains(toggle)) {
      const path = toggle.dataset.cfg!;
      write(path, configValue(snapshot, path) === "true" ? "false" : "true");
      return;
    }
    const choice = target.closest<HTMLElement>("[data-cfg] > button[data-v], [data-cfg] button[data-v]");
    const holder = choice?.closest<HTMLElement>("[data-cfg]");
    if (choice && holder && !(holder instanceof HTMLSelectElement) && root.contains(holder)) {
      const any = choice.dataset.any?.split(" ") ?? [];
      const current = configValue(snapshot, holder.dataset.cfg!) ?? "";
      if (any.includes(current)) return;
      write(holder.dataset.cfg!, choice.dataset.w ?? choice.dataset.v!);
      return;
    }
    const tab = target.closest<HTMLElement>(".seg[data-seg] button[data-tab]");
    if (tab) {
      setSeg(root, tab.closest<HTMLElement>(".seg")!.dataset.seg!, tab.dataset.tab!);
      return;
    }
    const jump = target.closest<HTMLElement>("[data-goto-tab]");
    if (jump) {
      const [name, name2] = jump.dataset.gotoTab!.split(":");
      setSeg(root, name, name2);
      return;
    }
    const go = target.closest<HTMLElement>("[data-goto]");
    if (go) {
      const [page, tab] = go.dataset.goto!.split(":");
      openPage(page, tab);
      return;
    }
    const libraryJump = target.closest<HTMLElement>("[data-lib-jump]");
    if (libraryJump) {
      openLibraryView(libraryJump.dataset.libJump!);
      return;
    }
    const conn = target.closest<HTMLElement>("[data-open-conn]");
    if (conn) {
      void import("./connections-drawer").then((m) => m.openConnections(conn.dataset.openConn ?? ""));
      return;
    }
    const nav = target.closest<HTMLElement>("[data-nav]");
    if (nav) {
      document.querySelector<HTMLElement>(`.sidebar .item[data-target="${nav.dataset.nav}"]`)?.click();
      return;
    }
    const attempt = target.closest<HTMLElement>("[data-try]");
    if (attempt) {
      postQuickAction("run", attempt.dataset.try);
      return;
    }
    const hotkey = target.closest<HTMLButtonElement>("button.hk[data-hk]");
    if (hotkey) beginCapture(hotkey);
  });

  root.addEventListener("change", (event) => {
    const node = event.target as HTMLElement;
    if (!node.matches("[data-cfg]")) return;
    if (node instanceof HTMLSelectElement || node instanceof HTMLInputElement || node instanceof HTMLTextAreaElement) {
      write(node.dataset.cfg!, node.value);
    }
  });

  const beginCapture = (button: HTMLButtonElement) => {
    const action = button.dataset.hk!;
    button.classList.add("capturing");
    button.textContent = "请按下新的快捷键…（Esc 取消，退格清除）";
    const finish = () => {
      button.classList.remove("capturing");
      document.removeEventListener("keydown", onKey, true);
      button.removeEventListener("blur", cancel);
      apply();
    };
    const cancel = () => finish();
    const onKey = (event: KeyboardEvent) => {
      event.preventDefault();
      event.stopPropagation();
      if (event.key === "Escape") return finish();
      if (event.key === "Backspace" || event.key === "Delete") {
        finish();
        write(`hotkeys.${action}`, "");
        return;
      }
      const combo = comboFromEvent(event);
      if (!combo) return;
      finish();
      const taken = Object.entries(snapshot.hotkeys ?? {}).find(
        ([key, value]) => key !== action && key !== "enabled" && value === combo,
      );
      if (taken) {
        const note = document.createElement("span");
        note.className = "chip bad hk-err";
        note.textContent = "这个组合已经用于另一个操作";
        button.after(note);
        window.setTimeout(() => note.remove(), 3000);
        return;
      }
      write(`hotkeys.${action}`, combo);
    };
    document.addEventListener("keydown", onKey, true);
    button.addEventListener("blur", cancel);
  };

  onHostMessage("configSnapshot", (message) => {
    snapshot = message.data as unknown as Snapshot;
    apply();
  });
  onHostMessage("quickActionResponse", (message) => {
    const hotkeys = message.hotkeys as typeof hotkeyState | undefined;
    if (hotkeys) {
      hotkeyState = hotkeys;
      paintHotkeyStatus();
    }
  });
  postQuickAction("status");

  return {
    value: read,
    raw,
    refresh: apply,
  };
}
