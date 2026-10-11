/**
 * @file src/clipboard.h
 * @brief Bounded text clipboard operations for authenticated streaming clients.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace clipboard {
  constexpr std::size_t max_bytes = 1024 * 1024;  ///< Maximum UTF-8 clipboard text length.
  constexpr unsigned version = 1;  ///< Text clipboard HTTPS protocol version.

  /**
   * @brief Result of a clipboard operation.
   */
  enum class status_e {
    ok,  ///< Operation completed.
    unavailable,  ///< The clipboard could not be opened or the platform is unsupported.
    too_large,  ///< Text exceeds the protocol byte limit.
    invalid_text,  ///< Text is malformed or contains an embedded null character.
    conflict  ///< The receiving clipboard changed after the sender captured its sequence.
  };

  /**
   * @brief A consistent text clipboard snapshot.
   */
  struct snapshot_t {
    status_e status = status_e::unavailable;  ///< Operation result.
    std::string text;  ///< UTF-8 text, possibly empty.
    uint32_t sequence = 0;  ///< Windows clipboard change sequence number.
    bool has_text = false;  ///< Whether the clipboard contains Unicode text.
  };

  /**
   * @brief Validate bounded UTF-8 text before allocating platform buffers.
   * @param text Text to validate.
   * @return Validation result; empty text is valid.
   */
  status_e validate_text(std::string_view text);

  /**
   * @brief Get the clipboard change counter without reading its payload.
   * @return Current sequence number, or zero on unsupported platforms.
   */
  uint32_t sequence();

  /**
   * @brief Read a bounded snapshot of the interactive user's text clipboard.
   * @return Text, sequence and format availability, or a failure status.
   */
  snapshot_t read_text();

  /**
   * @brief Replace the interactive user's clipboard with validated Unicode text.
   * @param text UTF-8 text to copy, including an empty string to clear text.
   * @param expected_sequence Optional destination generation checked while the clipboard is open.
   * @param written_sequence Optional output receiving the observed or published sequence before closing.
   * @return Operation result; conflicts leave newer clipboard content intact.
   */
  status_e write_text(std::string_view text, std::optional<uint32_t> expected_sequence = {}, uint32_t *written_sequence = nullptr);
}  // namespace clipboard
