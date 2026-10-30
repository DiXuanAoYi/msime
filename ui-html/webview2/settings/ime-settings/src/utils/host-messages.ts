import { isServerMessage, type ServerMessage } from '../../../../shared/messages';

const subscribers = new Set<(message: ServerMessage) => void>();
let listening = false;

// WebView2 dispatches each message once; parsing and schema traversal belong here,
// before fan-out to the lazily loaded settings modules.
export function onHostMessage<T extends ServerMessage['type']>(
  type: T,
  handler: (message: Extract<ServerMessage, { type: T }>) => void,
): () => void {
  const webview = window.chrome?.webview;
  if (!webview) return () => {};
  subscribers.add(deliver);
  if (!listening) {
    webview.addEventListener('message', (event: Event & { data?: unknown }) => {
      let payload = event.data;
      if (typeof payload === 'string') {
        try { payload = JSON.parse(payload); } catch { return; }
      }
      if (!isServerMessage(payload)) return;
      const message = payload;
      for (const subscriber of [...subscribers]) {
        try {
          subscriber(message);
        } catch {
          // One lazy module must not prevent unrelated modules receiving the same snapshot.
          // Report a fixed schema message type only, never an exception or message payload.
          window.dispatchEvent(new CustomEvent('settings-handler-error', { detail: { type: message.type } }));
        }
      }
    });
    listening = true;
  }
  function deliver(message: ServerMessage): void {
    if (message.type === type) handler(message as Extract<ServerMessage, { type: T }>);
  }
  return () => { subscribers.delete(deliver); };
}
