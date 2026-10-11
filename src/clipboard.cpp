/**
 * @file src/clipboard.cpp
 * @brief Windows Unicode clipboard transport with bounded allocations.
 */
#include "clipboard.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace clipboard {
  status_e validate_text(std::string_view text) {
    if (text.size() > max_bytes) {
      return status_e::too_large;
    }
    for (std::size_t i = 0; i < text.size();) {
      auto lead = static_cast<unsigned char>(text[i++]);
      if (lead == 0) {
        return status_e::invalid_text;
      }
      if (lead < 0x80) {
        continue;
      }
      unsigned remaining;
      uint32_t codepoint;
      uint32_t minimum;
      if (lead >= 0xc2 && lead <= 0xdf) {
        remaining = 1;
        codepoint = lead & 0x1f;
        minimum = 0x80;
      } else if (lead >= 0xe0 && lead <= 0xef) {
        remaining = 2;
        codepoint = lead & 0x0f;
        minimum = 0x800;
      } else if (lead >= 0xf0 && lead <= 0xf4) {
        remaining = 3;
        codepoint = lead & 0x07;
        minimum = 0x10000;
      } else {
        return status_e::invalid_text;
      }
      if (remaining > text.size() - i) {
        return status_e::invalid_text;
      }
      while (remaining--) {
        auto continuation = static_cast<unsigned char>(text[i++]);
        if ((continuation & 0xc0) != 0x80) {
          return status_e::invalid_text;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
      if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return status_e::invalid_text;
      }
    }
    return status_e::ok;
  }

#ifdef _WIN32
  namespace {
    /**
     * @brief Own a real message-only window for immediate-rendering clipboard writes.
     * @note The window is destroyed after the clipboard closes; no delayed rendering is used.
     */
    struct owner_t {
      HWND window = CreateWindowExW(0, L"STATIC", L"Sunshine Clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);  ///< Clipboard owner window.

      /**
       * @brief Destroy the owner on its creating thread.
       */
      ~owner_t() {
        if (window) {
          DestroyWindow(window);
        }
      }
    };

    /**
     * @brief Hold the clipboard open briefly, retrying contention for at most 100 ms.
     */
    struct open_t {
      owner_t owner;  ///< Per-operation window; HTTP workers do not pump persistent windows.
      bool opened = false;  ///< Whether this object owns an open clipboard.

      /**
       * @brief Open with a non-null owning window as required by SetClipboardData.
       */
      open_t() {
        if (!owner.window) {
          return;
        }
        for (unsigned attempt = 0; attempt < 10; ++attempt) {
          if (OpenClipboard(owner.window)) {
            opened = true;
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }

      /**
       * @brief Release the clipboard before completing the HTTP operation.
       */
      ~open_t() {
        if (opened) {
          CloseClipboard();
        }
      }
    };

    /** @brief Prepare an immediate format buffer before acquiring the clipboard. */
    struct allocation_t {
      HGLOBAL handle = nullptr;  ///< Movable buffer until Windows takes ownership.

      /** @brief Free a buffer whose ownership did not transfer. */
      ~allocation_t() {
        if (handle) {
          GlobalFree(handle);
        }
      }

      /** @brief Copy a bounded prepared format into its movable allocation. */
      bool prepare(const void *bytes, std::size_t length) {
        handle = GlobalAlloc(GMEM_MOVEABLE, length);
        auto memory = handle ? GlobalLock(handle) : nullptr;
        if (!memory) {
          return false;
        }
        std::memcpy(memory, bytes, length);
        GlobalUnlock(handle);
        return true;
      }

      /** @brief Transfer one prepared format while the caller holds the clipboard. */
      bool publish(UINT format) {
        if (!SetClipboardData(format, handle)) {
          return false;
        }
        handle = nullptr;
        return true;
      }
    };
  }  // namespace
#endif

  uint32_t sequence() {
#ifdef _WIN32
    return GetClipboardSequenceNumber();
#else
    return 0;
#endif
  }

  snapshot_t read_text() {
    snapshot_t result;
#ifdef _WIN32
    open_t clipboard;
    if (!clipboard.opened) {
      return result;
    }
    result.sequence = GetClipboardSequenceNumber();
    result.status = status_e::ok;
    if (IsClipboardFormatAvailable(CF_HDROP) || !IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      return result;
    }
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    const auto bytes = handle ? GlobalSize(handle) : 0;
    if (!bytes || bytes % sizeof(wchar_t) != 0) {
      result.status = status_e::invalid_text;
      return result;
    }
    const auto wide = static_cast<const wchar_t *>(GlobalLock(handle));
    if (!wide) {
      result.status = status_e::unavailable;
      return result;
    }
    // GlobalSize may include allocator padding. Scan a bounded prefix for the terminator.
    const auto count = std::min(bytes / sizeof(wchar_t), max_bytes + 1);
    const auto end = std::find(wide, wide + count, L'\0');
    if (end == wide + count) {
      GlobalUnlock(handle);
      result.status = bytes / sizeof(wchar_t) > max_bytes + 1 ? status_e::too_large : status_e::invalid_text;
      return result;
    }
    const auto length = static_cast<int>(end - wide);
    const int utf8_bytes = length ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, nullptr, 0, nullptr, nullptr) : 0;
    if (length && !utf8_bytes) {
      result.status = status_e::invalid_text;
    } else if (utf8_bytes > static_cast<int>(max_bytes)) {
      result.status = status_e::too_large;
    } else {
      result.text.resize(utf8_bytes);
      if (utf8_bytes) {
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, result.text.data(), utf8_bytes, nullptr, nullptr);
      }
      result.has_text = true;
    }
    GlobalUnlock(handle);
    result.sequence = GetClipboardSequenceNumber();
#endif
    return result;
  }

  status_e write_text(std::string_view text, std::optional<uint32_t> expected_sequence, uint32_t *written_sequence) {
    const auto validation = validate_text(text);
    if (validation != status_e::ok) {
      return validation;
    }
#ifdef _WIN32
    const int length = text.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!text.empty() && !length) {
      return status_e::invalid_text;
    }
    std::wstring wide(length, L'\0');
    if (length) {
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    }
    LCID locale = MAKELCID(LOWORD(GetKeyboardLayout(0)), SORT_DEFAULT);
    DWORD ansi_page = 0, oem_page = 0;
    auto code_pages = [&] {
      return GetLocaleInfoW(locale, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&ansi_page), sizeof(ansi_page) / sizeof(wchar_t)) &&
             GetLocaleInfoW(locale, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&oem_page), sizeof(oem_page) / sizeof(wchar_t));
    };
    if (!code_pages()) {
      locale = GetUserDefaultLCID();
      if (!code_pages()) {
        return status_e::unavailable;
      }
    }
    if (!ansi_page) {
      ansi_page = GetACP();
    }
    if (!oem_page) {
      oem_page = GetOEMCP();
    }
    allocation_t unicode_data, ansi_data, oem_data, locale_data;
    auto encoded = [&](allocation_t &allocation, UINT code_page) {
      const auto size = WideCharToMultiByte(code_page, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (!size) {
        return false;
      }
      std::vector<char> bytes(size);
      return WideCharToMultiByte(code_page, 0, wide.c_str(), -1, bytes.data(), size, nullptr, nullptr) == size && allocation.prepare(bytes.data(), bytes.size());
    };
    // CloseClipboard synthesizes missing text/locale formats and advances the
    // counter. Prepare all formats before opening so the guarded publication's
    // captured counter remains correct after close without an unprotected read.
    if (!unicode_data.prepare(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t)) || !encoded(ansi_data, ansi_page) || !encoded(oem_data, oem_page) || !locale_data.prepare(&locale, sizeof(locale))) {
      return status_e::unavailable;
    }
    open_t clipboard;
    if (!clipboard.opened) {
      return status_e::unavailable;
    }
    const auto current = GetClipboardSequenceNumber();
    if (written_sequence) {
      *written_sequence = current;
    }
    if (expected_sequence && *expected_sequence != current) {
      return status_e::conflict;
    }
    if (!EmptyClipboard() || !unicode_data.publish(CF_UNICODETEXT) || !ansi_data.publish(CF_TEXT) || !oem_data.publish(CF_OEMTEXT) || !locale_data.publish(CF_LOCALE)) {
      return status_e::unavailable;
    }
    if (written_sequence) {
      *written_sequence = GetClipboardSequenceNumber();
    }
    // Ownership of this immediate-rendering buffer has transferred to Windows.
    return status_e::ok;
#else
    return status_e::unavailable;
#endif
  }
}  // namespace clipboard
