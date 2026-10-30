/**
 * Jumps between settings pages for the quick pages: "更换" next to a model opens 语音与发音, a moved feature opens the
 * page that now owns it. The target tab may not exist yet (pages load on first visit), so the tab is looked up for a
 * short while after the sidebar click.
 */

const TAB_SELECTORS: Record<string, (tab: string) => string> = {
  voice: (tab) => `#voice [data-voice-target="${tab}"]`,
  "vision-bench": (tab) => `#visionPanel [data-vision-tab="${tab}"]`,
  vision: (tab) => `#visionSettings .seg button[data-tab="${tab}"]`,
  "reply-studio": (tab) => `#reply-studio-settings .seg button[data-tab="${tab}"]`,
  "personal-library": (tab) => `#personalLibraryPanel .seg button[data-tab="${tab}"]`,
};

function whenPresent(selector: string, run: (node: HTMLElement) => void): void {
  let tries = 0;
  const timer = window.setInterval(() => {
    const node = document.querySelector<HTMLElement>(selector);
    if (node || ++tries > 40) {
      window.clearInterval(timer);
      if (node) run(node);
    }
  }, 50);
}

export function openPage(page: string, tab?: string): void {
  document.querySelector<HTMLElement>(`.sidebar .item[data-target="${page}"]`)?.click();
  const selector = tab ? TAB_SELECTORS[page]?.(tab) : undefined;
  if (selector) whenPresent(selector, (node) => node.click());
}

/** 剪贴板页里已经搬走的几个视图，仍由同一份数据驱动；只有从别的页面跳过来时才显示它们的标签。 */
export function openLibraryView(view: string): void {
  openPage("personal-library");
  whenPresent(`#personalLibraryPanel [data-library-view="${view}"]`, (button) => {
    const root = document.getElementById("personalLibraryPanel");
    if (root) root.dataset.extra = "1";
    button.click();
  });
}

/** Going to the clipboard page from the sidebar always shows the plain clipboard again. */
export function resetLibraryExtras(): void {
  document.addEventListener("click", (event) => {
    const item = (event.target as HTMLElement).closest<HTMLElement>('.sidebar .item[data-target="personal-library"]');
    const root = document.getElementById("personalLibraryPanel");
    if (item && root && event.isTrusted) delete root.dataset.extra;
  });
}
