import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import "../styles/modules/prompt-library.css";
type Action =
  | "status"
  | "putGroup"
  | "removeGroup"
  | "putClip"
  | "putRule"
  | "favorite"
  | "remove"
  | "copy"
  | "review"
  | "playFavorite"
  | "copyFavorite"
  | "study";
const el = <T extends HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const value = (id: string) => el<HTMLInputElement>(id)?.value ?? "";
export function personalAction(
  action: Action,
  extra: Record<string, unknown> = {},
): void {
  window.chrome?.webview?.postMessage(
    serializeHostMessage({
      type: "personalLibraryRequest",
      data: {
        requestId: `library-${action}-${Date.now()}-${Math.random()}`,
        action,
        ...extra,
      },
    }),
  );
}
type Entry = {
  id: string;
  name: string;
  text?: string;
  group?: string;
  application?: string;
  code?: string;
  enabled?: boolean;
};
type Book = {
  groups: { id: string; name: string }[];
  clips: Entry[];
  rules: Entry[];
  favorites: Entry[];
};
export function setupPersonalLibrary(): void {
  const root = document.getElementById("personalLibraryPanel");
  if (!root || root.dataset.ready) return;
  root.dataset.ready = "1";
  let book: Book = { groups: [], clips: [], rules: [], favorites: [] },
    history: string[] = [],
    editing = "";
  let study: { word: string; language: string; gloss: string }[] = [],
    reviewed = 0;
  const status = (s: string) => {
    el("libraryStatus").textContent = s;
  };
  const button = (name: string, action: () => void) => {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = name;
    b.addEventListener("click", action);
    return b;
  };
  const reset = () => {
    editing = "";
    for (const id of ["libraryName", "libraryText", "libraryCode"])
      el<HTMLInputElement>(id).value = "";
    el<HTMLInputElement>("libraryEnabled").checked = true;
  };
  const card = () => {
    const c = document.createElement("div");
    c.className = "section voice-section";
    return c;
  };
  const render = () => {
    const view = value("libraryView"),
      isClip = view === "clips",
      isRule = view === "rules";
    el("libraryGroups").hidden = !isClip;
    el("libraryEditor").hidden = !isClip && !isRule;
    el("libraryAppFields").hidden = !isRule;
    el("libraryStudy").hidden = view !== "study";
    const group = el<HTMLSelectElement>("libraryGroup");
    const old = group.value;
    group.replaceChildren(...book.groups.map((g) => new Option(g.name, g.id)));
    group.value = book.groups.some((g) => g.id === old)
      ? old
      : (book.groups[0]?.id ?? "");
    const rows = el("libraryRows");
    rows.replaceChildren();
    if (view === "study") return;
    const entries: Entry[] =
      view === "history"
        ? history.map((text, i) => ({ id: String(i), name: "最近复制", text }))
        : view === "clips"
          ? book.clips.filter((x) => x.group === group.value)
          : view === "rules"
            ? book.rules
            : book.favorites;
    if (!entries.length) {
      const p = document.createElement("p");
      p.textContent =
        view === "favorites"
          ? "在语音工作台生成后点击“收藏语音”，即可在此重复播放或复制。"
          : "暂无内容。";
      rows.append(p);
    }
    for (const x of entries) {
      const c = card(),
        title = document.createElement("strong"),
        text = document.createElement("pre"),
        actions = document.createElement("div");
      title.textContent =
        x.name +
        (view === "rules"
          ? ` · ${x.application} / ${x.code}${x.enabled === false ? " · 已停用" : ""}`
          : "");
      text.textContent = x.text ?? "";
      text.style.cssText = "white-space:pre-wrap;overflow-wrap:anywhere";
      actions.className = "local-asr-actions";
      if (view === "favorites") {
        actions.append(
          button("播放", () => personalAction("playFavorite", { id: x.id })),
          button("复制音频文件", () =>
            personalAction("copyFavorite", { id: x.id }),
          ),
        );
      } else
        actions.append(
          button("复制", () => personalAction("copy", { text: x.text ?? "" })),
        );
      if (view === "clips" || view === "rules")
        actions.append(
          button("编辑", () => {
            editing = x.id;
            el<HTMLInputElement>("libraryName").value = x.name;
            el<HTMLTextAreaElement>("libraryText").value = x.text ?? "";
            if (view === "rules") {
              el<HTMLInputElement>("libraryApplication").value =
                x.application ?? "";
              el<HTMLInputElement>("libraryCode").value = x.code ?? "";
              el<HTMLInputElement>("libraryEnabled").checked =
                x.enabled !== false;
            }
            el("libraryEditor").scrollIntoView({ block: "start" });
          }),
        );
      if (view === "history")
        actions.append(
          button("加入常用语", () =>
            personalAction("putClip", {
              name: (x.text ?? "").slice(0, 30) || "收藏文本",
              text: x.text,
              group:
                book.groups.find((g) => g.id === "phrases")?.id ??
                book.groups[0]?.id,
            }),
          ),
        );
      if (view !== "history")
        actions.append(
          button("删除", () =>
            personalAction("remove", { section: view, id: x.id }),
          ),
        );
      c.append(title, text, actions);
      rows.append(c);
    }
  };
  const renderStudy = () => {
    const x = study[0];
    el("libraryStudyWord").textContent = x
      ? `${x.word} · ${x.language}`
      : "本轮复习完成或暂无记录";
    el("libraryStudyAnswer").textContent = x?.gloss ?? "";
    el("libraryStudyAnswer").hidden = true;
    el("libraryStudyCount").textContent =
      `本轮已复习 ${reviewed} 条 · 剩余 ${study.length} 条`;
    root
      .querySelectorAll<HTMLButtonElement>("[data-review]")
      .forEach((b) => (b.disabled = !x));
  };
  el("libraryRefresh").onclick = () => personalAction("status");
  el("libraryView").onchange = () => {
    reset();
    render();
  };
  el("libraryGroup").onchange = () => {
    reset();
    render();
  };
  el("libraryNew").onclick = reset;
  el("libraryAddGroup").onclick = () =>
    personalAction("putGroup", { name: value("libraryGroupName").trim() });
  el("libraryRenameGroup").onclick = () =>
    personalAction("putGroup", {
      id: value("libraryGroup"),
      name: value("libraryGroupName").trim(),
    });
  el("libraryRemoveGroup").onclick = () =>
    personalAction("removeGroup", { id: value("libraryGroup") });
  el("librarySave").onclick = () => {
    const rule = value("libraryView") === "rules";
    // Keep the identity across repeated saves, including a failed validation retry.
    if (!editing) editing = crypto.randomUUID();
    personalAction(rule ? "putRule" : "putClip", {
      id: editing,
      name: value("libraryName").trim(),
      text: value("libraryText"),
      ...(rule
        ? {
            application: value("libraryApplication").trim(),
            code: value("libraryCode").trim(),
            enabled: el<HTMLInputElement>("libraryEnabled").checked,
          }
        : { group: value("libraryGroup") }),
    });
  };
  el("libraryLoadStudy").onclick = () => {
    reviewed = 0;
    personalAction("study");
  };
  el("libraryReveal").onclick = () => {
    el("libraryStudyAnswer").hidden = false;
  };
  root.querySelectorAll<HTMLButtonElement>("[data-review]").forEach(
    (b) =>
      (b.onclick = () => {
        const x = study[0];
        if (!x) return;
        personalAction("review", {
          text: x.word,
          language: x.language,
          grade: Number(b.dataset.review),
        });
        root
          .querySelectorAll<HTMLButtonElement>("[data-review]")
          .forEach((b) => (b.disabled = true));
      }),
  );
  onHostMessage("personalLibraryResponse", (message) => {
    if (!message.requestId.startsWith("library-")) return;
    if (!message.ok) {
      status(message.message);
      renderStudy();
      return;
    }
    if (message.requestId.startsWith("library-review-")) {
      study.shift();
      reviewed++;
      renderStudy();
    }
    book = (message.book ?? book) as unknown as Book;
    history = message.history ?? history;
    if (Array.isArray(message.study)) {
      study = message.study as typeof study;
      renderStudy();
    }
    status(message.message || "已保存到本机");
    render();
  });
  personalAction("status");
  renderStudy();
}
