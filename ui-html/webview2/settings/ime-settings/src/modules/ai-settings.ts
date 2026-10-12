import {setupNeuralModels} from './neural-models';
import { setupPromptLibrary, applyPromptLibrary } from './prompt-library';
import { applyDropdownValue, applyToggleState, setupDropdownMenu, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { setupCredentialTest } from './credential-test';
import { setupModelFetch } from './model-fetch';
import { setupAiWorkbench, applyAiTaskConfig } from './ai-workbench';

type ProviderDefaults = { endpoint: string; model: string };

const fields: Record<string, string> = {
  aiToken: 'token', aiEndpoint: 'endpoint', aiModel: 'model'
};

const PROVIDER_DEFAULTS: Record<string, ProviderDefaults> = {
  deepseek: {
    endpoint: 'https://api.deepseek.com/chat/completions',
    model: 'deepseek-v4-flash'
  },
  openai: {
    endpoint: 'https://api.openai.com/v1/chat/completions',
    model: 'gpt-4o-mini'
  },
  siliconflow: {
    endpoint: 'https://api.siliconflow.cn/v1/chat/completions',
    model: 'Qwen/Qwen3-8B'
  },
  groq: {
    endpoint: 'https://api.groq.com/openai/v1/chat/completions',
    model: 'llama-3.3-70b-versatile'
  },
  custom: {endpoint:'',model:''},
  local: {endpoint:'http://127.0.0.1:11434/v1/chat/completions',model:''
  }
};

const PROVIDERS = ['deepseek', 'openai', 'siliconflow', 'groq','custom','local'] as const;
let tokens: Record<string, string> = {};
let endpoints: Record<string, string> = {};
let models: Record<string, string> = {};
let currentProvider = 'deepseek';
let resetModels=()=>{};
function applyProviderVisibility():void {
  document.getElementById('aiThinkingField')?.classList.toggle('is-hidden',currentProvider!=='deepseek');
  const token=document.getElementById('aiToken') as HTMLInputElement|null;
  if(token)token.placeholder=currentProvider==='local'?'本地服务可留空':'';
  const note=document.getElementById('aiConnectionNote');if(note)note.textContent=currentProvider==='local'?'仅连接回环地址；本机文本服务需要提前启动。':'模型列表失败时仍可手动填写模型名。';
}

function applyProviderFields(provider: string): void {
  const defaults = PROVIDER_DEFAULTS[provider];
  if (!defaults) return;
  const endpoint = document.getElementById('aiEndpoint') as HTMLInputElement | null;
  const model = document.getElementById('aiModel') as HTMLInputElement | null;
  // 空值和 Server 一致地表示提供商默认值
  if (endpoint) endpoint.value = endpoints[provider] || defaults.endpoint;
  if (model) {
    model.value = models[provider] || defaults.model;
    model.placeholder = defaults.model;
  }
}

function readProviderMap(raw: unknown, provider: string, legacyValue: string): Record<string, string> {
  const result: Record<string, string> = {};
  if (raw && typeof raw === 'object') {
    Object.entries(raw as Record<string, unknown>).forEach(([key, value]) => {
      if (typeof value === 'string') result[key] = value;
    });
  }
  if (legacyValue && !result[provider]) result[provider] = legacyValue;
  return result;
}

function switchProvider(provider: string): void {
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  if (token) {
    tokens[currentProvider] = token.value.trim();
    updateConfig(`ai_assistant.token_${currentProvider}`, tokens[currentProvider]);
  }
  for (const [id, key, slots] of [
    ['aiEndpoint', 'endpoint', endpoints], ['aiModel', 'model', models]
  ] as const) {
    const input = document.getElementById(id) as HTMLInputElement | null;
    if (input) {
      slots[currentProvider] = input.value;
      updateConfig(`ai_assistant.${key}`, input.value);
    }
  }
  currentProvider = provider;
  if (token) token.value = tokens[provider] ?? '';
  updateConfig('ai_assistant.provider', provider);
  applyProviderFields(provider);
  applyProviderVisibility();resetModels();
}

export function setupAiSettings(): void {
  setupPromptLibrary('association','aiPromptLibrary','aiPrompt');
  setupAiWorkbench();
  setupNeuralModels();
  setupToggleButton('aiEnabled', value => updateConfig('ai_assistant.enabled', value));
  setupTokenVisibilityToggle();
  const connectionConfig=():Record<string,string>=>{
    const defaults = PROVIDER_DEFAULTS[currentProvider];
    const value = (id: string) =>
      (document.getElementById(id) as HTMLInputElement | null)?.value.trim() ?? '';
    return {
      provider: currentProvider,
      token: value('aiToken'),
      endpoint: value('aiEndpoint') || defaults?.endpoint || '',
      model: value('aiModel') || defaults?.model || ''
    };
  };
  setupCredentialTest('aiCredentialTestButton','aiCredentialTestStatus',()=> 'ai.assistant',connectionConfig);
  resetModels=setupModelFetch({inputId:'aiModel',arrowId:'aiModelArrow',menuId:'aiModelMenu',buttonId:'aiModelFetchButton',statusId:'aiModelFetchStatus',service:()=> 'ai.assistant',readConfig:connectionConfig});
  setupDropdownMenu('aiThinkingBtn','aiThinkingMenu','',true,'ai_assistant.thinking');
  setupDropdownMenu('aiProviderBtn', 'aiProviderMenu', 'changeAiProvider', true);
  document.getElementById('aiProviderMenu')?.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement | null)?.closest('.dropdown-item') as HTMLElement | null;
    const provider = item?.dataset.value;
    if (provider) switchProvider(provider);
  });
  Object.entries(fields).forEach(([id, key]) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLTextAreaElement | null;
    element?.addEventListener('change', () => {
      if (id === 'aiToken') {
        tokens[currentProvider] = element.value.trim();
        updateConfig(`ai_assistant.token_${currentProvider}`, tokens[currentProvider]);
        resetModels();
        return;
      }
      updateConfig(`ai_assistant.${key}`, element.value);
      if(id==='aiEndpoint')resetModels();
      if (!element.value) {
        const defaults = PROVIDER_DEFAULTS[currentProvider];
        if (defaults) element.value = defaults[key as keyof ProviderDefaults];
      }
    });
  });
  const limit = document.getElementById('aiCandidateLimit') as HTMLInputElement | null;
  limit?.addEventListener('change', () => {
    const value = Math.max(1, Math.min(10, Number.parseInt(limit.value, 10) || 3));
    limit.value = String(value);
    updateConfig('ai_assistant.candidate_limit', value);
  });
}

function setupTokenVisibilityToggle(): void {
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  const toggle = document.getElementById('aiTokenVisibility') as HTMLButtonElement | null;
  if (!token || !toggle) return;

  toggle.addEventListener('click', () => {
    const shouldShow = token.type === 'password';
    token.type = shouldShow ? 'text' : 'password';
    toggle.setAttribute('aria-pressed', String(shouldShow));

    const label = shouldShow ? '隐藏 API Token' : '显示 API Token';
    toggle.setAttribute('aria-label', label);
    toggle.title = label;
  });
}

export function applyAiConfig(config: Record<string, unknown>): void {
  Object.entries(fields).forEach(([id, key]) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLTextAreaElement | null;
    if (element && typeof config[key] === 'string') element.value = config[key] as string;
  });
  const limit = document.getElementById('aiCandidateLimit') as HTMLInputElement | null;
  if (limit && typeof config.candidate_limit === 'number') limit.value = String(config.candidate_limit);
  if (typeof config.enabled === 'boolean') applyToggleState('aiEnabled', config.enabled);
  currentProvider = typeof config.provider === 'string' ? config.provider : 'deepseek';
  tokens = readProviderMap(config.tokens, currentProvider, typeof config.token === 'string' ? config.token : '');
  endpoints = readProviderMap(config.endpoints, currentProvider, typeof config.endpoint === 'string' ? config.endpoint : '');
  models = readProviderMap(config.models, currentProvider, typeof config.model === 'string' ? config.model : '');
  PROVIDERS.forEach((provider) => {
    const slot = config[`token_${provider}`];
    if (typeof slot === 'string' && slot) tokens[provider] = slot;
  });
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  if (token) token.value = tokens[currentProvider] ?? '';
  applyDropdownValue('aiProviderBtn', 'aiProviderMenu', currentProvider);
  const defaults = PROVIDER_DEFAULTS[currentProvider];
  const model = document.getElementById('aiModel') as HTMLInputElement | null;
  if (model && defaults) model.placeholder = defaults.model;
  applyPromptLibrary('association', config);
  if(typeof config.thinking==='string')applyDropdownValue('aiThinkingBtn','aiThinkingMenu',config.thinking);
  applyProviderVisibility();applyAiTaskConfig(config);
}
