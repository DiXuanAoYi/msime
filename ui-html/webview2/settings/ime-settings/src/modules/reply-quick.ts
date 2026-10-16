import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import { bindQuickControls } from "./quick-controls";

export interface Tone {
  id: string;
  name: string;
  desc: string;
  example: string;
  on: boolean;
  custom: boolean;
}

export interface Persona {
  fillers: string[];
  banned: string[];
  emoji: "none" | "some" | "many";
  punct: "normal" | "less" | "none";
  address: "contact" | "ni" | "nin";
  about: string;
}

export interface Contact {
  id: string;
  name: string;
  platform: string;
  relation: string;
  tone: string;
  address: string;
  length: string;
  notes: string;
  summary: string;
  retentionDays: number;
  messageCount: number;
  updated: number;
}

const SAMPLE = "好的，我明天上午十点之前把方案发给您。如果有需要调整的地方，请随时告诉我。";

/** What the persona does to a plain model reply, shown next to the original so the effect is obvious. */
export function personaPreview(sample: string, persona: Persona): string {
  let text = sample;
  if (persona.address === "ni") text = text.replace(/您/g, "你");
  else if (persona.address === "nin") text = text.replace(/你/g, "您");
  for (const word of persona.banned) if (word) text = text.split(word).join("");
  if (persona.punct === "none") text = text.replace(/[，。！？、]/g, " ").replace(/\s+/g, " ").trim();
  else if (persona.punct === "less") text = text.replace(/。(?=.)/g, "，").replace(/。$/, "");
  const filler = persona.fillers[0];
  if (filler) text = `${filler}，${text}`;
  if (persona.emoji === "some") text += " 🙂";
  else if (persona.emoji === "many") text += " 🙂👍";
  return text;
}

export function moveTone(tones: Tone[], index: number, delta: -1 | 1): Tone[] {
  const target = index + delta;
  if (index < 0 || index >= tones.length || target < 0 || target >= tones.length) return tones;
  const next = tones.slice();
  [next[index], next[target]] = [next[target], next[index]];
  return next;
}

export function newToneId(existing: Tone[], now = Date.now()): string {
  let id = `custom-${now.toString(36)}`;
  let n = 0;
  while (existing.some((tone) => tone.id === id)) id = `custom-${now.toString(36)}-${++n}`;
  return id;
}

export function addWord(words: string[], raw: string, limit: number): string[] {
  const word = raw.trim().slice(0, 36);
  if (!word || words.includes(word) || words.length >= limit) return words;
  return [...words, word];
}

const COLORS = ["#5b57b8", "#2f8f6b", "#c0702b", "#3b82c4", "#b04a7a", "#7a6a2a"];
export const avatarColor = (name: string) =>
  COLORS[[...name].reduce((sum, c) => sum + c.charCodeAt(0), 0) % COLORS.length];

export function setupReplyQuick(): void {
  const root = document.getElementById("reply-studio-settings");
  if (!root || root.dataset.quickReady) return;
  root.dataset.quickReady = "1";
  const controls = bindQuickControls(root, (path) => {
    if (path === "reply.reader_enabled") paintReader();
  });
  const $ = <T extends HTMLElement>(id: string) => root.querySelector<T>(`#${id}`)!;

  let tones: Tone[] = [];
  let persona: Persona = { fillers: [], banned: [], emoji: "some", punct: "normal", address: "contact", about: "" };
  let contacts: Contact[] = [];
  let recordCount = 0;
  let selected = "";
  let editing: Contact | null = null;
  let serial = 0;

  const request = (action: string, extra: Record<string, unknown> = {}) =>
    window.chrome?.webview?.postMessage(
      serializeHostMessage({
        type: "replyProfileRequest",
        data: { requestId: `rp-${action}-${++serial}`, action: action as "get", ...extra },
      }),
    );

  const paintReader = () => {
    const on = controls.value("reply.reader_enabled") === "true";
    const chip = $("rpReaderChip");
    chip.textContent = `读取聊天：${on ? "开" : "关"}`;
    chip.className = `chip ${on ? "good" : ""}`;
  };

  // ---------------------------------------------------------------- tones
  const toneForm = $<HTMLFormElement>("toneForm");
  const renderTones = () => {
    const list = $("toneList");
    list.replaceChildren();
    tones.forEach((tone, index) => {
      const row = document.createElement("div");
      row.className = `tone${tone.on ? "" : " off"}`;
      const move = document.createElement("div");
      move.className = "tone-move";
      for (const [label, delta] of [["▲", -1], ["▼", 1]] as const) {
        const b = document.createElement("button");
        b.type = "button";
        b.textContent = label;
        b.setAttribute("aria-label", delta < 0 ? "上移" : "下移");
        b.onclick = () => saveTones(moveTone(tones, index, delta));
        move.append(b);
      }
      const main = document.createElement("div");
      main.className = "tone-main";
      const name = document.createElement("b");
      name.textContent = tone.name;
      if (tone.custom) {
        const chip = document.createElement("span");
        chip.className = "chip plain accent";
        chip.textContent = "自定义";
        name.append(chip);
      }
      const desc = document.createElement("small");
      desc.textContent = tone.desc;
      main.append(name, desc);
      if (tone.example) {
        const ex = document.createElement("span");
        ex.className = "ex";
        ex.textContent = `例：${tone.example}`;
        main.append(ex);
      }
      const right = document.createElement("div");
      right.className = "tone-r";
      if (tone.custom) {
        const del = document.createElement("button");
        del.type = "button";
        del.className = "btn sm ghost danger";
        del.textContent = "删除";
        del.onclick = () => saveTones(tones.filter((t) => t.id !== tone.id));
        right.append(del);
      }
      const toggle = document.createElement("button");
      toggle.type = "button";
      toggle.className = "tg";
      toggle.setAttribute("role", "switch");
      toggle.setAttribute("aria-label", `开启${tone.name}`);
      toggle.setAttribute("aria-checked", String(tone.on));
      toggle.onclick = () => saveTones(tones.map((t) => (t.id === tone.id ? { ...t, on: !t.on } : t)));
      right.append(toggle);
      row.append(move, main, right);
      list.append(row);
    });
  };
  const saveTones = (next: Tone[]) => {
    if (!next.some((tone) => tone.on)) {
      window.alert("至少要保留一种语气。");
      return;
    }
    tones = next;
    renderTones();
    request("putTones", { tones: next.map(({ id, on, custom, name, desc, example }) => (custom ? { id, on, custom, name, desc, example } : { id, on })) });
  };
  $("toneAdd").onclick = () => {
    toneForm.hidden = !toneForm.hidden;
    if (!toneForm.hidden) $<HTMLInputElement>("toneName").focus();
  };
  $("toneCancel").onclick = () => {
    toneForm.hidden = true;
    toneForm.reset();
  };
  toneForm.onsubmit = (event) => {
    event.preventDefault();
    const name = $<HTMLInputElement>("toneName").value.trim();
    const desc = $<HTMLTextAreaElement>("toneDesc").value.trim();
    if (!name || !desc) {
      window.alert("请填写名称和“怎么回”的说明。");
      return;
    }
    saveTones([...tones, { id: newToneId(tones), name, desc, example: $<HTMLInputElement>("toneEx").value.trim(), on: true, custom: true }]);
    toneForm.hidden = true;
    toneForm.reset();
  };

  // ---------------------------------------------------------------- persona
  const renderWords = (hostId: string, key: "fillers" | "banned", limit: number, placeholder: string) => {
    const host = $(hostId);
    host.replaceChildren();
    for (const word of persona[key]) {
      const tag = document.createElement("span");
      tag.className = "tagx";
      tag.textContent = word;
      const x = document.createElement("button");
      x.type = "button";
      x.textContent = "×";
      x.setAttribute("aria-label", `删除${word}`);
      x.onclick = () => savePersona({ ...persona, [key]: persona[key].filter((w) => w !== word) });
      tag.append(x);
      host.append(tag);
    }
    const input = document.createElement("input");
    input.type = "text";
    input.className = "tag-add";
    input.placeholder = placeholder;
    input.maxLength = 36;
    input.onkeydown = (event) => {
      if (event.key !== "Enter") return;
      event.preventDefault();
      savePersona({ ...persona, [key]: addWord(persona[key], input.value, limit) });
    };
    host.append(input);
  };
  const renderPersona = () => {
    renderWords("fillerChips", "fillers", 10, "＋ 回车添加");
    renderWords("bannedChips", "banned", 20, "＋ 回车添加");
    for (const [id, key] of [["personaEmoji", "emoji"], ["personaPunct", "punct"], ["personaAddress", "address"]] as const) {
      $(id).querySelectorAll<HTMLElement>("button[data-v]").forEach((b) => {
        b.setAttribute("aria-pressed", String(b.dataset.v === persona[key]));
      });
    }
    const about = $<HTMLTextAreaElement>("aboutMe");
    if (document.activeElement !== about) about.value = persona.about;
    $("personaAfter").textContent = personaPreview(SAMPLE, persona);
  };
  const savePersona = (next: Persona) => {
    persona = next;
    renderPersona();
    request("putPersona", { persona: next });
  };
  for (const [id, key] of [["personaEmoji", "emoji"], ["personaPunct", "punct"], ["personaAddress", "address"]] as const) {
    $(id).addEventListener("click", (event) => {
      const b = (event.target as HTMLElement).closest<HTMLElement>("button[data-v]");
      if (b) savePersona({ ...persona, [key]: b.dataset.v } as Persona);
    });
  }
  $<HTMLTextAreaElement>("aboutMe").addEventListener("change", (event) => {
    savePersona({ ...persona, about: (event.target as HTMLTextAreaElement).value.slice(0, 600) });
  });

  // ---------------------------------------------------------------- contacts
  const renderContacts = () => {
    const list = $("ctList");
    const needle = $<HTMLInputElement>("ctQuery").value.trim().toLowerCase();
    list.replaceChildren();
    const shown = contacts.filter((c) => !needle || c.name.toLowerCase().includes(needle) || c.platform.toLowerCase().includes(needle));
    if (!shown.length) {
      const empty = document.createElement("div");
      empty.className = "empty";
      empty.textContent = contacts.length ? "没有匹配的联系人。" : "还没有联系人。打开“读取聊天”后会自动建立，也可以手动添加。";
      list.append(empty);
    }
    for (const contact of shown) {
      const item = document.createElement("button");
      item.type = "button";
      item.className = "ct-item";
      item.setAttribute("aria-current", String(contact.id === selected));
      const av = document.createElement("span");
      av.className = "av sm";
      av.style.background = avatarColor(contact.name);
      av.textContent = [...contact.name][0] ?? "?";
      const text = document.createElement("span");
      const name = document.createElement("b");
      name.textContent = contact.name;
      const small = document.createElement("small");
      small.textContent = `${contact.platform || "手动添加"}${contact.relation ? " · " + contact.relation : ""}`;
      text.append(name, small);
      item.append(av, text);
      item.onclick = () => {
        selected = contact.id;
        editing = { ...contact };
        renderContacts();
        renderEditor();
      };
      list.append(item);
    }
  };
  const field = (label: string, control: HTMLElement) => {
    const wrap = document.createElement("label");
    wrap.className = "field";
    wrap.append(label, control);
    return wrap;
  };
  const select = (options: Array<[string, string]>, value: string, onChange: (v: string) => void) => {
    const s = document.createElement("select");
    for (const [v, text] of options) {
      const o = document.createElement("option");
      o.value = v;
      o.textContent = text;
      s.append(o);
    }
    s.value = value;
    s.onchange = () => onChange(s.value);
    return s;
  };
  const renderEditor = () => {
    const host = $("ctEdit");
    host.replaceChildren();
    const contact = editing;
    if (!contact) {
      const hint = document.createElement("p");
      hint.className = "muted";
      hint.textContent = "选择左侧的联系人，或点“添加”。";
      host.append(hint);
      return;
    }
    const head = document.createElement("div");
    head.className = "ct-edit-h";
    const av = document.createElement("span");
    av.className = "av";
    av.style.background = avatarColor(contact.name || "?");
    av.textContent = [...(contact.name || "?")][0];
    const title = document.createElement("div");
    const h3 = document.createElement("h3");
    h3.textContent = contact.name || "新联系人";
    const meta = document.createElement("span");
    meta.className = "small muted";
    meta.textContent = `${contact.platform || "手动添加"} · ${contact.messageCount ?? 0} 条聊天记录`;
    title.append(h3, meta);
    head.append(av, title);

    const name = document.createElement("input");
    name.type = "text";
    name.maxLength = 120;
    name.value = contact.name;
    name.oninput = () => (contact.name = name.value);
    const relation = document.createElement("input");
    relation.type = "text";
    relation.maxLength = 40;
    relation.placeholder = "例如：直属领导、客户、同学";
    relation.value = contact.relation;
    relation.oninput = () => (contact.relation = relation.value);
    const notes = document.createElement("textarea");
    notes.rows = 2;
    notes.maxLength = 600;
    notes.placeholder = "例如：说话简洁，不喜欢长段文字；周五不开会";
    notes.value = contact.notes;
    notes.oninput = () => (contact.notes = notes.value);
    const summary = document.createElement("textarea");
    summary.rows = 3;
    summary.maxLength = 1500;
    summary.placeholder = "聊过的重要事情。可在工作台里用“总结上下文”生成后粘贴到这里。";
    summary.value = contact.summary;
    summary.oninput = () => (contact.summary = summary.value);

    const toneOptions: Array<[string, string]> = [["", "跟随面板顺序"], ...tones.filter((t) => t.on).map((t) => [t.id, t.name] as [string, string])];
    const grid = document.createElement("div");
    grid.className = "grid2";
    grid.append(
      field("名称", name),
      field("关系", relation),
      field("默认语气", select(toneOptions, contact.tone, (v) => (contact.tone = v))),
      field("称呼", select([["", "按我的习惯"], ["您", "用“您”"], ["你", "用“你”"]], contact.address, (v) => (contact.address = v))),
      field("回复长度", select([["", "跟随默认"], ["short", "简短"], ["medium", "适中"], ["long", "详细"]], contact.length, (v) => (contact.length = v))),
      field("聊天记录保留", select([["30", "30 天"], ["90", "90 天"], ["365", "一年"]], String(contact.retentionDays || 90), (v) => (contact.retentionDays = Number(v)))),
    );

    const actions = document.createElement("div");
    actions.className = "row-r left";
    const save = document.createElement("button");
    save.type = "button";
    save.className = "btn sm primary";
    save.textContent = "保存";
    save.onclick = () => {
      if (!contact.name.trim()) {
        window.alert("请填写联系人名称。");
        return;
      }
      request("putContact", { contact });
    };
    const exportBtn = document.createElement("button");
    exportBtn.type = "button";
    exportBtn.className = "btn sm";
    exportBtn.textContent = "导出（明文）";
    exportBtn.onclick = () => request("export", { id: contact.id });
    const del = document.createElement("button");
    del.type = "button";
    del.className = "btn sm danger";
    del.textContent = "删除";
    del.onclick = () => {
      if (window.confirm(`删除“${contact.name}”的记忆和聊天记录？`)) {
        selected = "";
        editing = null;
        request("deleteContact", { id: contact.id });
        renderEditor();
      }
    };
    actions.append(save, exportBtn, del);
    host.append(head, grid, field("备注", notes), field("记忆摘要", summary), actions);
  };
  $("ctAdd").onclick = () => {
    const id = `手动添加|${Date.now().toString(36)}`;
    editing = { id, name: "", platform: "手动添加", relation: "", tone: "", address: "", length: "", notes: "", summary: "", retentionDays: 90, messageCount: 0, updated: 0 };
    selected = "";
    renderContacts();
    renderEditor();
  };
  $<HTMLInputElement>("ctQuery").addEventListener("input", renderContacts);

  // ---------------------------------------------------------------- reader records
  const summary = $("rdSummary");
  const renderRecords = () => {
    summary.textContent = `${contacts.length} 位联系人 · ${recordCount} 条聊天记录`;
  };
  const confirm = $("readerClearConfirm");
  $("readerClear").onclick = () => (confirm.hidden = false);
  $("readerClearNo").onclick = () => (confirm.hidden = true);
  $("readerClearYes").onclick = () => {
    confirm.hidden = true;
    request("clearRecords");
  };

  onHostMessage("replyProfileResponse", (message) => {
    if (!message.requestId.startsWith("rp-")) return;
    if (!message.ok) {
      window.alert(message.message || "保存失败");
      request("get");
      return;
    }
    if (Array.isArray(message.tones)) tones = message.tones as unknown as Tone[];
    if (message.persona) persona = message.persona as unknown as Persona;
    if (Array.isArray(message.contacts)) contacts = message.contacts as unknown as Contact[];
    recordCount = message.recordCount ?? recordCount;
    if (message.action === "putContact" && message.id) {
      selected = message.id;
      editing = contacts.find((c) => c.id === selected) ?? editing;
    }
    if (message.action === "export" && typeof message.exported === "string") {
      void navigator.clipboard?.writeText(message.exported).then(
        () => window.alert("已复制到剪贴板（明文 JSON），请自己保管。"),
        () => window.alert(message.exported as string),
      );
    }
    if (editing && selected) editing = contacts.find((c) => c.id === selected) ?? editing;
    renderTones();
    renderPersona();
    renderContacts();
    renderEditor();
    renderRecords();
  });
  onHostMessage("configSnapshot", () => paintReader());
  root.addEventListener("qp-tab", (event) => {
    const detail = (event as CustomEvent<{ name: string; tab: string }>).detail;
    if (detail.name === "reply" && ["tones", "persona", "contacts", "reader"].includes(detail.tab)) request("get");
  });
  request("get");
  paintReader();
  renderEditor();
}
