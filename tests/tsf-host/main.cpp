// Manual, process-local TSF fixture. It never injects keys or exports secret
// controls.
#include <InputScope.h>
#include <Windows.h>
#include <algorithm>
#include <msctf.h>
#include <string>

#ifndef MSIME_TSF_HOST_STATE_PATH
#define MSIME_TSF_HOST_STATE_PATH L""
#endif

namespace {
HWND first, second, password, pin, status;
HFONT font;
HMODULE scopeModule;
HRESULT activationResult = E_PENDING;
HRESULT scopeResults[4] = {E_PENDING, E_PENDING, E_PENDING, E_PENDING};
const CLSID service = {0xe3062e9a,
                       0xd834,
                       0x4637,
                       {0x89, 0x58, 0xed, 0x8c, 0xfa, 0x42, 0x7d, 0x01}};
const GUID profile = {0x4d59b1b4,
                      0xd503,
                      0x44ae,
                      {0x92, 0x59, 0xba, 0xd9, 0xbb, 0x27, 0x78, 0xab}};
std::wstring GuidText(const GUID &value) {
  wchar_t result[40]{};
  StringFromGUID2(value, result, ARRAYSIZE(result));
  return result;
}
bool ActiveProfile(TF_INPUTPROCESSORPROFILE &value) {
  ITfInputProcessorProfileMgr *manager = nullptr;
  HRESULT hr = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr, reinterpret_cast<void **>(&manager));
  if (SUCCEEDED(hr) && manager) {
    hr = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &value);
    manager->Release();
  }
  return hr == S_OK;
}
bool IsWaterProfile(const TF_INPUTPROCESSORPROFILE &value) {
  return value.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
         IsEqualGUID(value.clsid, service) &&
         IsEqualGUID(value.guidProfile, profile);
}
void RefreshStatus() {
  TF_INPUTPROCESSORPROFILE value{};
  const bool known = ActiveProfile(value);
  wchar_t text[512]{};
  swprintf_s(
      text,
      L"%s | %s | 激活 0x%08X | 输入配置文件 "
      L"%s\r\n服务端维护拼音/五笔/日语方案；本夹具不读取含 API 凭据的配置。",
      sizeof(void *) == 8 ? L"x64" : L"x86",
      known ? (IsWaterProfile(value) ? L"当前确实选中水杉"
                                     : L"当前选中其他键盘/输入法")
            : L"当前配置文件读取失败",
      static_cast<unsigned>(activationResult),
      known ? GuidText(value.guidProfile).c_str() : L"未知");
  SetWindowTextW(status, text);
}
void Activate() {
  ITfInputProcessorProfileMgr *manager = nullptr;
  activationResult = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr, reinterpret_cast<void **>(&manager));
  if (SUCCEEDED(activationResult) && manager) {
    activationResult =
        manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service,
                                 profile, nullptr, TF_IPPMF_FORPROCESS);
    manager->Release();
  }
  RefreshStatus();
}
std::wstring ReadNormal(HWND control) {
  const int length = (std::min)(GetWindowTextLengthW(control), 4096);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int read = GetWindowTextW(control, text.data(), length + 1);
  text.resize(static_cast<size_t>((std::max)(0, read)));
  return text;
}
std::string JsonString(const std::wstring &value) {
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                        static_cast<int>(value.size()), nullptr,
                                        0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>((std::max)(0, bytes)), '\0');
  if (bytes)
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), utf8.data(), bytes,
                        nullptr, nullptr);
  std::string json = "\"";
  const char hex[] = "0123456789abcdef";
  for (const unsigned char c : utf8) {
    if (c == '"' || c == '\\') {
      json += '\\';
      json += static_cast<char>(c);
    } else if (c < 0x20) {
      json += "\\u00";
      json += hex[c >> 4];
      json += hex[c & 15];
    } else
      json += static_cast<char>(c);
  }
  return json + '"';
}
void ExportState(HWND window) {
  const wchar_t *path = MSIME_TSF_HOST_STATE_PATH;
  if (!path[0]) {
    MessageBoxW(window, L"构建时未指定测试输出路径，未保存任何内容。",
                L"导出测试状态", MB_OK);
    return;
  }
  TF_INPUTPROCESSORPROFILE value{};
  const bool known = ActiveProfile(value);
  // Deliberately whitelist the two ordinary controls. Never read password or
  // PIN text.
  const auto ordinary1 = ReadNormal(first), ordinary2 = ReadNormal(second);
  std::string json =
      "{\n  \"processBitness\":" + std::to_string(sizeof(void *) * 8) +
      ",\n  \"activationHresult\":" +
      std::to_string(static_cast<unsigned>(activationResult)) +
      ",\n  \"waterProfileActive\":" +
      (known && IsWaterProfile(value) ? "true" : "false") +
      ",\n  \"activeProfileGuid\":" +
      JsonString(known ? GuidText(value.guidProfile) : L"") +
      ",\n  \"activeServiceClsid\":" +
      JsonString(known ? GuidText(value.clsid) : L"") +
      ",\n  \"normalText1\":" + JsonString(ordinary1) +
      ",\n  \"normalText2\":" + JsonString(ordinary2) +
      ",\n  \"inputScopeHresults\":[" +
      std::to_string(static_cast<unsigned>(scopeResults[0])) + "," +
      std::to_string(static_cast<unsigned>(scopeResults[1])) + "," +
      std::to_string(static_cast<unsigned>(scopeResults[2])) + "," +
      std::to_string(static_cast<unsigned>(scopeResults[3])) +
      "],\n  \"secretControlValuesExported\":false\n}\n";
  const HANDLE file =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written = 0;
  const bool saved =
      file != INVALID_HANDLE_VALUE &&
      WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written,
                nullptr) &&
      written == json.size();
  if (file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
  MessageBoxW(
      window,
      saved
          ? L"已保存普通框文本和实际 TSF 配置文件 GUID；未读取或导出密码/PIN。"
          : L"测试文件保存失败。",
      L"导出测试状态", MB_OK | (saved ? MB_ICONINFORMATION : MB_ICONERROR));
}
HWND Label(HWND window, const wchar_t *text, int y) {
  HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, 20, y,
                               900, 27, window, nullptr, nullptr, nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  return control;
}
HWND Edit(HWND window, int id, int y, int height, DWORD style, InputScope scope,
          int index) {
  HWND control = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 20, y, 890, height, window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  const auto setScopes =
      scopeModule ? reinterpret_cast<decltype(&SetInputScopes)>(
                        GetProcAddress(scopeModule, "SetInputScopes"))
                  : nullptr;
  scopeResults[index] =
      setScopes ? setScopes(control, &scope, 1, nullptr, 0, nullptr, nullptr)
                : E_NOTIMPL;
  return control;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
  case WM_CREATE:
    font = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 10, 900,
                           60, window, nullptr, nullptr, nullptr);
    Label(window,
          L"普通输入区一：拼音/英文候选、三语切换、Shift+Enter、最近上屏撤销",
          77);
    first = Edit(window, 101, 108, 105,
                 ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, IS_TEXT, 0);
    Label(window, L"普通输入区二：失焦/回焦、跨框上下文测试", 223);
    second = Edit(window, 102, 254, 92,
                  ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, IS_TEXT, 1);
    Label(window, L"密码测试框（IS_PASSWORD）：只用测试口令；不会导出内容",
          355);
    password = Edit(window, 105, 386, 37, ES_AUTOHSCROLL | ES_PASSWORD,
                    IS_PASSWORD, 2);
    Label(window,
          L"数字口令测试框（IS_NUMERIC_PASSWORD）：只用测试数字；不会导出内容",
          432);
    pin = Edit(window, 106, 463, 37, ES_AUTOHSCROLL | ES_PASSWORD | ES_NUMBER,
               IS_NUMERIC_PASSWORD, 3);
    CreateWindowW(L"BUTTON", L"重新激活测试输入法",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 520, 250, 35, window,
                  reinterpret_cast<HMENU>(103), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"导出测试状态（不含密码/PIN）",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP, 285, 520, 340, 35, window,
                  reinterpret_cast<HMENU>(104), nullptr, nullptr);
    Activate();
    SetFocus(first);
    SetTimer(window, 1, 1000, nullptr);
    return 0;
  case WM_TIMER:
    RefreshStatus();
    return 0;
  case WM_COMMAND:
    if (LOWORD(wparam) == 103 && HIWORD(wparam) == BN_CLICKED) {
      Activate();
      SetFocus(first);
      return 0;
    }
    if (LOWORD(wparam) == 104 && HIWORD(wparam) == BN_CLICKED) {
      ExportState(window);
      return 0;
    }
    break;
  case WM_DESTROY:
    KillTimer(window, 1);
    DeleteObject(font);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return 1;
  scopeModule =
      LoadLibraryExW(L"msctf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  WNDCLASSW wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"MsimeTsfSmokeHost";
  wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);
  HWND window = CreateWindowW(wc.lpszClassName,
                              sizeof(void *) == 8 ? L"水杉 TSF 实机测试 x64"
                                                  : L"水杉 TSF 实机测试 x86",
                              WS_OVERLAPPEDWINDOW, 120, 80, 950, 625, nullptr,
                              nullptr, instance, nullptr);
  ShowWindow(window, show);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  CoUninitialize();
  if (scopeModule)
    FreeLibrary(scopeModule);
  return 0;
}
