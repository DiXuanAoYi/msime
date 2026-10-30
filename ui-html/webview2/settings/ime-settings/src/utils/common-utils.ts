import { dismissHoistedOverlays } from './overlay-host';

// HTML 加载工具
// Prefer bundling partials so they work in WebView2/file://
const partials = import.meta.glob<string>('/src/partials/**/*.html', {
  query: '?raw',
  import: 'default',
});

export async function loadHTML(url: string): Promise<string> {
  if (url.startsWith('/src/partials/')) {
    const loader = partials[url];
    if (!loader) {
      throw new Error(`Partial not found: ${url}`);
    }
    return await loader();
  }

  const response = await fetch(url);
  return await response.text();
}

const MODULE_IDS = [
  'floating-toolbar',
  'stats',
  'appearance',
  'input',
  'helpcode',
  'dict',
  'skin',
  'voice',
  'screenkb-settings',
  'handwriting-settings',
  'tools-settings',
  'ai-settings',
  'reply-studio',
  'personal-library',
  'vision',
  'vision-bench',
  'shortcut',
  'help-settings',
  'about-settings',
  'feedback-settings',
] as const;

let activeModuleName: string | null = null;

// 只显示当前模块。首次调用会一次性隐藏其余模块；之后只切换上一页 / 下一页。
export function showOnlyCurrentModule(moduleName: string): void {
  if (activeModuleName === moduleName) {
    return;
  }

  const next = document.getElementById(moduleName);
  if (!next) {
    return;
  }

  if (activeModuleName === null) {
    for (const id of MODULE_IDS) {
      if (id === moduleName) {
        continue;
      }
      const el = document.getElementById(id);
      if (el) {
        el.style.display = 'none';
      }
    }
  } else {
    const prev = document.getElementById(activeModuleName);
    if (prev) {
      prev.style.display = 'none';
    }
  }

  next.style.display = 'block';
  // 各子页面共用同一个滚动容器 #content-container，切页时需要把滚动位置归零，
  // 否则新页面会停留在上一个页面的滚动偏移处。
  const scrollContainer = document.getElementById('content-container');
  if (scrollContainer) {
    scrollContainer.scrollTop = 0;
  }
  activeModuleName = moduleName;
  // 词库 / 实用功能的浮层挂在 body 下（见 hoistOverlay），不随模块容器一起隐藏，切页时收起
  dismissHoistedOverlays();
}
