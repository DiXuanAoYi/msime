// Keep the scroll extent and complete searchable data, but bound the live DOM.
export const LIBRARY_ROW_HEIGHT = 136;
export const LIBRARY_COMPACT_ROW_HEIGHT = 70;
const VIRTUAL_THRESHOLD = 80;
const OVERSCAN = 6;
const MAX_VISIBLE_ROWS = 48;

export function libraryWindow(total: number, scrollTop: number, viewport: number, rowHeight = LIBRARY_ROW_HEIGHT) {
  const count = Math.max(0, Math.floor(total));
  const height = Number.isFinite(viewport) && viewport > 0 ? viewport : 520;
  const top = Number.isFinite(scrollTop) ? Math.max(0, scrollTop) : 0;
  const first = Math.min(Math.max(0, count - 1), Math.floor(top / rowHeight));
  const start = Math.max(0, first - OVERSCAN);
  const size = Math.min(MAX_VISIBLE_ROWS, Math.ceil(height / rowHeight) + 1 + OVERSCAN * 2);
  return {start, end: Math.min(count, start + size)};
}

export class VirtualLibraryList<T> {
  private items: readonly T[] = [];
  private createRow: ((item: T, index: number) => HTMLElement) | undefined;
  private mounted = new Map<number, HTMLElement>();
  private frame: number | undefined;
  private generation = 0;
  private virtual = false;
  private rowHeight = LIBRARY_ROW_HEIGHT;
  private observer: ResizeObserver | undefined;
  private readonly container: HTMLElement;

  constructor(container: HTMLElement) {
    this.container = container;
    container.addEventListener('scroll', this.schedule, {passive: true});
    if (typeof ResizeObserver !== 'undefined') {
      this.observer = new ResizeObserver(this.schedule);
      this.observer.observe(container);
    }
  }

  private cancel() {
    this.generation++;
    if (this.frame !== undefined) window.cancelAnimationFrame(this.frame);
    this.frame = undefined;
  }

  private schedule = () => {
    if (!this.virtual || this.frame !== undefined) return;
    const generation = this.generation;
    this.frame = window.requestAnimationFrame(() => {
      // A category/search change invalidates any previously scheduled work.
      if (generation !== this.generation) return;
      this.frame = undefined;
      this.draw();
    });
  };

  setItems(items: readonly T[], createRow: (item: T, index: number) => HTMLElement, rowHeight = LIBRARY_ROW_HEIGHT) {
    this.cancel();
    this.items = items;
    this.rowHeight = rowHeight;
    this.container.style.setProperty('--library-row-height', `${rowHeight}px`);
    this.createRow = createRow;
    this.mounted.clear();
    this.virtual = items.length > VIRTUAL_THRESHOLD;
    this.container.classList.toggle('is-virtual', this.virtual);
    this.container.replaceChildren();
    if (this.virtual) {
      const extent = document.createElement('div');
      extent.className = 'clipboard-scroll-extent';
      extent.setAttribute('aria-hidden', 'true');
      extent.style.height = `${items.length * this.rowHeight}px`;
      this.container.append(extent);
      this.container.scrollTop = Math.min(this.container.scrollTop, Math.max(0, items.length * this.rowHeight - this.container.clientHeight));
    }
    this.draw();
  }

  clear() {
    this.cancel();
    this.items = [];
    this.createRow = undefined;
    this.mounted.clear();
    this.virtual = false;
    this.container.classList.toggle('is-virtual', false);
    this.container.replaceChildren();
  }

  private draw() {
    const {start, end} = this.virtual
      ? libraryWindow(this.items.length, this.container.scrollTop, this.container.clientHeight, this.rowHeight)
      : {start: 0, end: this.items.length};
    // Keep keyboard focus alive when the user scrolls its row out of view.
    const focused = [...this.mounted].find(([, row]) => row.contains(document.activeElement))?.[0];
    for (const [index, row] of this.mounted) {
      if ((index < start || index >= end) && index !== focused) {
        row.remove();
        this.mounted.delete(index);
      }
    }
    for (let index = start; index < end; index++) {
      if (this.mounted.has(index)) continue;
      const row = this.createRow!(this.items[index], index);
      row.setAttribute('aria-posinset', String(index + 1));
      row.setAttribute('aria-setsize', String(this.items.length));
      if (this.virtual) row.style.top = `${index * this.rowHeight}px`;
      const next = [...this.mounted].filter(([i]) => i > index).sort(([a], [b]) => a - b)[0]?.[1];
      this.container.insertBefore(row, next ?? null);
      this.mounted.set(index, row);
    }
  }

  focusRow(index: number) {
    if (index < 0 || index >= this.items.length) return;
    if (this.virtual) {
      const top = index * this.rowHeight;
      const bottom = top + this.rowHeight;
      const viewport = this.container.clientHeight || 520;
      if (top < this.container.scrollTop) this.container.scrollTop = top;
      else if (bottom > this.container.scrollTop + viewport) this.container.scrollTop = bottom - viewport;
      this.draw();
    }
    const row = this.mounted.get(index);
    row?.click();
    // Also reveal the row in the settings page's outer scroll container.
    row?.focus();
    // Release the previous focused row after moving focus to the new window.
    if (this.virtual) this.draw();
  }

  dispose() {
    this.clear();
    this.container.removeEventListener('scroll', this.schedule);
    this.observer?.disconnect();
  }
}
