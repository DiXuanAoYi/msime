// Manual smoke-test host: real Win32 EDIT controls and process-local TSF
// profile activation. It neither registers an IME nor changes the user's
// default input profile.
#include <Windows.h>
#include <msctf.h>
#include <string>

namespace {
HWND first, second, status;
HFONT font;
const CLSID service = {0xe3062e9a,
                       0xd834,
                       0x4637,
                       {0x89, 0x58, 0xed, 0x8c, 0xfa, 0x42, 0x7d, 0x01}};
const GUID profile = {0x4d59b1b4,
                      0xd503,
                      0x44ae,
                      {0x92, 0x59, 0xba, 0xd9, 0xbb, 0x27, 0x78, 0xab}};
void Activate() {
  ITfInputProcessorProfileMgr *manager = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr, reinterpret_cast<void **>(&manager));
  if (SUCCEEDED(result)) {
    result =
        manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service,
                                 profile, nullptr, TF_IPPMF_FORPROCESS);
    manager->Release();
  }
  wchar_t text[120];
  swprintf_s(text, L"TSF profile activation: 0x%08X — %s host",
             static_cast<unsigned>(result),
             sizeof(void *) == 8 ? L"x64" : L"x86");
  SetWindowText(status, text);
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
  case WM_CREATE: {
    font = CreateFont(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    status = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 15, 900,
                          32, window, nullptr, nullptr, nullptr);
    CreateWindow(L"STATIC",
                 L"输入区一：拼音、选词、翻页、退格、Ctrl+Enter 译词",
                 WS_CHILD | WS_VISIBLE, 20, 56, 900, 28, window, nullptr,
                 nullptr, nullptr);
    first = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                               ES_AUTOVSCROLL | WS_VSCROLL,
                           20, 90, 890, 170, window,
                           reinterpret_cast<HMENU>(101), nullptr, nullptr);
    CreateWindow(L"STATIC", L"输入区二：用于失焦 / 回焦验证",
                 WS_CHILD | WS_VISIBLE, 20, 278, 890, 28, window, nullptr,
                 nullptr, nullptr);
    second = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                ES_AUTOVSCROLL | WS_VSCROLL,
                            20, 310, 890, 160, window,
                            reinterpret_cast<HMENU>(102), nullptr, nullptr);
    CreateWindow(L"BUTTON", L"重新激活测试输入法",
                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 490, 250, 36, window,
                 reinterpret_cast<HMENU>(103), nullptr, nullptr);
    SendMessage(first, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(second, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    Activate();
    SetFocus(first);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wparam) == 103) {
      Activate();
      SetFocus(first);
    }
    break;
  case WM_DESTROY:
    DeleteObject(font);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(window, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASS wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"MsimeTsfSmokeHost";
  wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClass(&wc);
  const auto window = CreateWindow(
      wc.lpszClassName,
      sizeof(void *) == 8 ? L"水杉 TSF 实机测试 x64" : L"水杉 TSF 实机测试 x86",
      WS_OVERLAPPEDWINDOW, 120, 100, 950, 590, nullptr, nullptr, instance,
      nullptr);
  ShowWindow(window, show);
  UpdateWindow(window);
  MSG message{};
  while (GetMessage(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessage(&message);
  }
  CoUninitialize();
  return 0;
}
