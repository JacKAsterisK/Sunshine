/**
 * @file tests/unit/test_clipboard.cpp
 * @brief Validate clipboard text boundaries without modifying the user's clipboard.
 */
#include "../tests_common.h"

#include <algorithm>
#include <array>
#include <future>
#include <iterator>
#include <src/clipboard.h>
#include <src/config.h>
#include <src/rtsp.h>
#include <stdexcept>
#include <string>

#ifdef _WIN32
// clang-format off
  // Windows types must precede the shell's DROPFILES declaration.
  #include <windows.h>
  #include <shlobj.h>
// clang-format on
#endif

TEST(ClipboardTextTest, AcceptsEmptyAsciiAndAllUnicodeWidths) {
  EXPECT_EQ(clipboard::validate_text(""), clipboard::status_e::ok);
  EXPECT_EQ(clipboard::validate_text("clipboard\r\ntext"), clipboard::status_e::ok);
  EXPECT_EQ(clipboard::validate_text("\xc2\x80\xe0\xa0\x80\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"), clipboard::status_e::ok);
}

TEST(ClipboardTextTest, CountsUtf8BytesAndAllowsExactLimit) {
  const std::string text(clipboard::max_bytes, 'a');
  EXPECT_EQ(clipboard::validate_text(text), clipboard::status_e::ok);
  EXPECT_EQ(clipboard::validate_text(text + 'a'), clipboard::status_e::too_large);
  // Validation runs before any call to Windows or any clipboard replacement.
  EXPECT_EQ(clipboard::write_text(text + 'a'), clipboard::status_e::too_large);
}

TEST(ClipboardTextTest, RejectsMalformedUtf8AndEmbeddedNulls) {
  const std::array malformed {
    std::string("\0", 1),
    std::string("a\0b", 3),
    std::string("\x80"),
    std::string("\xc0\x80"),
    std::string("\xc1\xbf"),
    std::string("\xc2"),
    std::string("\xc2\x41"),
    std::string("\xe0\x80\x80"),
    std::string("\xed\xa0\x80"),
    std::string("\xf0\x80\x80\x80"),
    std::string("\xf4\x90\x80\x80"),
    std::string("\xf5\x80\x80\x80"),
    std::string("\xff"),
    std::string("\xe2\x82"),
    std::string("\xf0\x90\x80")
  };
  for (const auto &text : malformed) {
    EXPECT_EQ(clipboard::validate_text(text), clipboard::status_e::invalid_text);
    EXPECT_EQ(clipboard::write_text(text), clipboard::status_e::invalid_text);
  }
}

TEST(ClipboardTextTest, ClipboardIsOffByDefault) {
  EXPECT_FALSE(config::input.clipboard);
  EXPECT_FALSE(config::input.clipboard_files);
}

TEST(ClipboardTextTest, AbsentStreamNeverRunsClipboardAction) {
  bool called = false;
  EXPECT_FALSE(rtsp_stream::with_active_session("", [&]() {
    called = true;
  }));
  EXPECT_FALSE(rtsp_stream::with_active_session("unknown certificate", [&]() {
    called = true;
  }));
  EXPECT_FALSE(rtsp_stream::active_session_id("unknown certificate"));
  EXPECT_FALSE(rtsp_stream::with_active_session("unknown certificate", 123, [&]() {
    called = true;
  }));
  EXPECT_FALSE(called);
}

#ifdef _WIN32
namespace {
  /**
   * @brief Isolate native clipboard tests from every interactive user's window station.
   * @note A newly created named station is mandatory; no anonymous/current-station fallback is allowed.
   */
  class isolated_clipboard_t {
    HWINSTA original_station = GetProcessWindowStation();  ///< Borrowed station restored before worker exit.
    HDESK original_desktop = GetThreadDesktop(GetCurrentThreadId());  ///< Borrowed worker desktop.
    HWINSTA station = nullptr;  ///< Newly created private station.
    HDESK desktop = nullptr;  ///< Newly created private desktop within that station.
    bool station_changed = false;  ///< Whether process station restoration is necessary.
    bool desktop_changed = false;  ///< Whether thread desktop restoration is necessary.

    /**
     * @brief Verify a station or desktop handle refers to the requested unique named object.
     * @param handle User object handle.
     * @param expected Unique object name.
     * @return True only when the handle's object name exactly matches.
     */
    static bool named(HANDLE handle, const std::wstring &expected) {
      DWORD bytes = 0;
      GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &bytes);
      if (!bytes || bytes > 65536 || bytes % sizeof(wchar_t)) {
        return false;
      }
      std::wstring actual(bytes / sizeof(wchar_t), L'\0');
      if (!GetUserObjectInformationW(handle, UOI_NAME, actual.data(), bytes, &bytes)) {
        return false;
      }
      const auto end = actual.find(L'\0');
      if (end != std::wstring::npos) {
        actual.resize(end);
      }
      return actual == expected;
    }

  public:
    /**
     * @brief Create and select an isolated clipboard station on this dedicated test thread.
     * @return Empty on success, or a safe reason to skip before any clipboard operation.
     */
    std::optional<std::string> prepare() {
      if (!original_station || !original_desktop) {
        return "Cannot capture worker's original window station and desktop";
      }
      const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(GetTickCount64());
      const auto station_name = L"SunshineClipboardTestStation-" + suffix;
      SetLastError(ERROR_SUCCESS);
      station = CreateWindowStationW(station_name.c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
      const auto created_error = GetLastError();
      if (!station || created_error == ERROR_ALREADY_EXISTS || !named(station, station_name)) {
        return "Cannot create a new uniquely named private window station";
      }
      if (!SetProcessWindowStation(station)) {
        return "Cannot select the private test window station";
      }
      station_changed = true;
      const auto desktop_name = L"SunshineClipboardTestDesktop-" + suffix;
      SetLastError(ERROR_SUCCESS);
      desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
      const auto desktop_error = GetLastError();
      if (!desktop || desktop_error == ERROR_ALREADY_EXISTS || !named(desktop, desktop_name)) {
        return "Cannot create a new uniquely named private desktop";
      }
      if (!SetThreadDesktop(desktop)) {
        return "Cannot select the private test desktop";
      }
      desktop_changed = true;
      if (!named(GetProcessWindowStation(), station_name) || !named(GetThreadDesktop(GetCurrentThreadId()), desktop_name)) {
        return "Worker is not attached to the isolated named test station and desktop";
      }
      return {};
    }

    /**
     * @brief Restore borrowed identities on every return/exception before closing owned test objects.
     */
    ~isolated_clipboard_t() {
      // Returning to the original thread desktop requires restoring its owning process station first.
      if (station_changed && !SetProcessWindowStation(original_station)) {
        std::terminate();
      }
      if (desktop_changed && !SetThreadDesktop(original_desktop)) {
        std::terminate();
      }
      if (desktop) {
        CloseDesktop(desktop);
      }
      if (station) {
        CloseWindowStation(station);
      }
    }
  };

  /**
   * @brief Prepare real clipboard ownership only after private-station isolation succeeded.
   */
  class test_clipboard_owner_t {
    HWND window = nullptr;  ///< Immediate-rendering clipboard owner window.
    bool opened = false;  ///< Whether clipboard closure is required.

  public:
    /**
     * @brief Open the isolated clipboard with a real owner window.
     */
    test_clipboard_owner_t() {
      window = CreateWindowExW(0, L"STATIC", L"Sunshine isolated clipboard test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
      opened = window && OpenClipboard(window);
    }

    /**
     * @brief Release clipboard ownership and the per-operation window.
     */
    ~test_clipboard_owner_t() {
      if (opened) {
        CloseClipboard();
      }
      if (window) {
        DestroyWindow(window);
      }
    }

    /**
     * @brief Report whether the isolated clipboard can be modified by this owner.
     * @return True if a real owner successfully opened it.
     */
    bool available() const {
      return opened;
    }
  };

  /**
   * @brief Retain allocated test clipboard memory until ownership passes to Windows.
   */
  struct test_clipboard_memory_t {
    HGLOBAL handle;  ///< Movable buffer freed unless ownership transferred.

    /**
     * @brief Allocate a bounded immediate-rendering clipboard format buffer.
     * @param bytes Required allocation length.
     */
    explicit test_clipboard_memory_t(std::size_t bytes):
        handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes)) {}

    /**
     * @brief Free a still-owned format buffer.
     */
    ~test_clipboard_memory_t() {
      if (handle) {
        GlobalFree(handle);
      }
    }
  };
}  // namespace

TEST(ClipboardTextTest, WindowsBackendUsesOnlyAnIsolatedNamedWindowStation) {
  auto worker = std::async(std::launch::async, []() -> std::optional<std::string> {
    isolated_clipboard_t isolation;
    if (auto skipped = isolation.prepare()) {
      return skipped;
    }
    // Every clipboard call below operates in the newly named private station.
    const std::string unicode = "clipboard \xc3\xa9 \xe6\xbc\xa2 \xf0\x9f\x90\xb8";
    const auto initial = clipboard::sequence();
    uint32_t published = 0;
    EXPECT_EQ(clipboard::write_text(unicode, initial, &published), clipboard::status_e::ok);
    EXPECT_NE(published, initial);
    EXPECT_EQ(published, clipboard::sequence());
    auto snapshot = clipboard::read_text();
    EXPECT_EQ(snapshot.status, clipboard::status_e::ok);
    EXPECT_TRUE(snapshot.has_text);
    EXPECT_EQ(snapshot.text, unicode);
    EXPECT_EQ(snapshot.sequence, published);
    {
      test_clipboard_owner_t owner;
      if (!owner.available()) {
        throw std::runtime_error("Cannot inspect isolated native text formats");
      }
      // Missing conversions are registered by CloseClipboard and used to
      // change the captured generation after a successful native publication.
      for (auto format : {CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT, CF_LOCALE}) {
        EXPECT_TRUE(IsClipboardFormatAvailable(format));
        EXPECT_NE(GetClipboardData(format), nullptr);
      }
      EXPECT_EQ(clipboard::sequence(), published);
    }
    EXPECT_EQ(clipboard::sequence(), published);

    uint32_t local = 0;
    EXPECT_EQ(clipboard::write_text("newer local text", published, &local), clipboard::status_e::ok);
    uint32_t observed = 0;
    EXPECT_EQ(clipboard::write_text("stale remote text", published, &observed), clipboard::status_e::conflict);
    EXPECT_EQ(observed, local);
    EXPECT_EQ(clipboard::sequence(), local);
    snapshot = clipboard::read_text();
    EXPECT_EQ(snapshot.text, "newer local text");
    EXPECT_EQ(snapshot.sequence, local);

    EXPECT_EQ(clipboard::write_text("", local, &published), clipboard::status_e::ok);
    snapshot = clipboard::read_text();
    EXPECT_EQ(snapshot.status, clipboard::status_e::ok);
    EXPECT_TRUE(snapshot.has_text);
    EXPECT_TRUE(snapshot.text.empty());
    EXPECT_EQ(snapshot.sequence, published);
    EXPECT_EQ(published, clipboard::sequence());
    {
      test_clipboard_owner_t owner;
      if (!owner.available()) {
        throw std::runtime_error("Cannot inspect isolated empty text formats");
      }
      for (auto format : {CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT, CF_LOCALE}) {
        EXPECT_TRUE(IsClipboardFormatAvailable(format));
      }
      EXPECT_EQ(clipboard::sequence(), published);
    }
    EXPECT_EQ(clipboard::sequence(), published);

    {
      test_clipboard_owner_t owner;
      if (!owner.available() || !EmptyClipboard()) {
        throw std::runtime_error("Cannot prepare isolated clipboard format fixture");
      }
      constexpr wchar_t text[] = L"Explorer also supplied Unicode text";
      test_clipboard_memory_t text_memory(sizeof(text));
      auto wide_text = static_cast<wchar_t *>(GlobalLock(text_memory.handle));
      if (!wide_text) {
        throw std::runtime_error("Cannot lock isolated text fixture");
      }
      std::copy(std::begin(text), std::end(text), wide_text);
      GlobalUnlock(text_memory.handle);
      if (!SetClipboardData(CF_UNICODETEXT, text_memory.handle)) {
        throw std::runtime_error("Cannot publish isolated text fixture");
      }
      text_memory.handle = nullptr;
      // A syntactic local file selection is sufficient: this test never opens the named file.
      constexpr wchar_t paths[] = L"C:\\SunshineClipboardFixture\\copied.txt\0";
      test_clipboard_memory_t drop_memory(sizeof(DROPFILES) + sizeof(paths));
      auto drop = static_cast<DROPFILES *>(GlobalLock(drop_memory.handle));
      if (!drop) {
        throw std::runtime_error("Cannot lock isolated file-selection fixture");
      }
      drop->pFiles = sizeof(DROPFILES);
      drop->fWide = TRUE;
      std::copy(std::begin(paths), std::end(paths), reinterpret_cast<wchar_t *>(reinterpret_cast<unsigned char *>(drop) + sizeof(DROPFILES)));
      GlobalUnlock(drop_memory.handle);
      if (!SetClipboardData(CF_HDROP, drop_memory.handle)) {
        throw std::runtime_error("Cannot publish isolated file-selection fixture");
      }
      drop_memory.handle = nullptr;
    }
    snapshot = clipboard::read_text();
    EXPECT_EQ(snapshot.status, clipboard::status_e::ok);
    EXPECT_FALSE(snapshot.has_text);
    EXPECT_TRUE(snapshot.text.empty());
    EXPECT_EQ(snapshot.sequence, clipboard::sequence());
    return {};
  });
  std::optional<std::string> skipped;
  ASSERT_NO_THROW(skipped = worker.get());
  if (skipped) {
    GTEST_SKIP() << *skipped;
  }
}
#endif
