/**
 * @file src/clipboard_files.h
 * @brief Authenticated, session-bound asynchronous Windows file clipboard transfers.
 */
#pragma once

#include "clipboard_files_engine.h"

#include <functional>
#include <memory>

namespace clipboard_files {
  /**
   * @brief A short authorization action executed under pairing and session locks.
   */
  using guard_t = std::function<bool(const std::function<void()> &)>;

  /**
   * @brief Bounded HTTP response; bodies never contain local filesystem paths.
   */
  struct response_t {
    int status = 400;  ///< HTTP status code.
    std::string body;  ///< Binary manifest/batch or an opaque transfer identity.
    uint32_t sequence = 0;  ///< Interactive clipboard sequence observed by this operation.
    std::string id;  ///< Opaque transfer identity, exposed as a response header.
  };

#ifdef _WIN32
  /**
   * @brief Injectable desktop operations, permitting isolated tests without changing the clipboard.
   */
  struct desktop_t {
    std::function<std::shared_ptr<void>()> enter_context;  ///< Optional scoped interactive-user impersonation for request and background I/O.
    std::function<clipboard_snapshot_t()> read = read_clipboard_files;  ///< Capture only local CF_HDROP roots.
    std::function<uint32_t()> sequence = [] {
      return GetClipboardSequenceNumber();
    };  ///< Read change generation.
    std::function<uint32_t(const std::vector<std::filesystem::path> &, uint32_t)> publish;  ///< Optional injectable publisher; an empty callback uses prepared native CF_HDROP.
  };

  /**
   * @brief Own asynchronous snapshots/stages in a fixed private local directory.
   */
  class service_t {
  public:
    /**
     * @brief Construct a service with local storage and desktop backend.
     * @param storage Absolute per-user staging directory.
     * @param desktop Interactive desktop operations.
     */
    explicit service_t(std::filesystem::path storage, desktop_t desktop = {});
    /**
     * @brief Cancel and join outstanding work before releasing unpublished stages.
     */
    ~service_t();
    /**
     * @brief Process one authenticated operation without holding authorization locks for bulk I/O.
     * @param owner Canonical paired certificate plus exact active stream generation.
     * @param guard Revalidate current pairing and that stream before short guarded actions.
     * @param op Operation name.
     * @param id Opaque transfer identity; cannot select local paths.
     * @param batch Deterministic batch number.
     * @param sequence Expected destination sequence for commit, or since for offer.
     * @param body Bounded binary request body.
     * @return HTTP result and clipboard/transfer headers.
     */
    response_t request(const std::string &owner, const guard_t &guard, std::string_view op, std::string_view id, uint32_t batch, uint32_t sequence, std::string_view body);

  private:
    class impl_t;  ///< Private session registry and worker ownership implementation.
    std::unique_ptr<impl_t> impl;  ///< Owning per-user transfer service state.
  };
#endif

  /**
   * @brief Dispatch to the process-wide interactive file transfer service.
   * @param owner Canonical certificate plus current launch generation.
   * @param guard Short authorization/publication guard.
   * @param op Protocol operation name.
   * @param id Opaque transfer identity.
   * @param batch Batch number.
   * @param sequence Since or expected destination sequence.
   * @param body Bounded request payload.
   * @return HTTP response; unsupported platforms return 404.
   */
  response_t request(const std::string &owner, const guard_t &guard, std::string_view op, std::string_view id, uint32_t batch, uint32_t sequence, std::string_view body);
}  // namespace clipboard_files
