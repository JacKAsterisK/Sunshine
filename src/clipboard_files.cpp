/**
 * @file src/clipboard_files.cpp
 * @brief Session-bound bulk clipboard service with asynchronous staging and verification.
 */
#include "clipboard_files.h"

#include <future>
#include <optional>
#include <unordered_map>
#ifdef _WIN32
  #include <sddl.h>
  #include <WtsApi32.h>
#endif

namespace clipboard_files {
#ifdef _WIN32
  /**
   * @brief Restore the caller's prior security context even when file I/O throws.
   */
  class impersonation_t {
    handle_t previous;  ///< Prior impersonation token, or an invalid handle for process identity.
    bool restore = false;  ///< Whether the prior effective security context must be restored.

  public:
    /**
     * @brief Enter an interactive-user identity after capturing the caller's prior token.
     * @param token User token to impersonate for scoped file work.
     */
    explicit impersonation_t(HANDLE token):
        previous([] {
          HANDLE handle = INVALID_HANDLE_VALUE;
          if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &handle)) {
            require(GetLastError() == ERROR_NO_TOKEN, "Cannot capture file security context");
          }
          return handle;
        }()) {
      require(ImpersonateLoggedOnUser(token), "Cannot enter interactive-user file security context");
      restore = true;
    }

    /**
     * @brief Restore the previous thread token, failing closed if restoration is impossible.
     */
    ~impersonation_t() {
      if (restore) {
        bool restored = previous.value != INVALID_HANDLE_VALUE ? ImpersonateLoggedOnUser(previous.value) != 0 : RevertToSelf() != 0;
        if (!restored) {
          std::terminate();  // Never continue file I/O with the wrong effective identity.
        }
      }
    }
  };

  /**
   * @brief Token identity includes logon generation so a new login cannot reuse old transfers.
   */
  std::string token_identity(HANDLE token) {
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    require(bytes > 0 && bytes < 65536, "Cannot query interactive-user identity");
    std::vector<unsigned char> buffer(bytes);
    require(GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes), "Cannot query interactive-user identity");
    auto user = reinterpret_cast<TOKEN_USER *>(buffer.data());
    require(!IsWellKnownSid(user->User.Sid, WinLocalSystemSid), "No interactive-user file identity");
    wchar_t *sid = nullptr;
    require(ConvertSidToStringSidW(user->User.Sid, &sid), "Cannot encode interactive-user identity");
    auto identity = utf8(sid);
    LocalFree(sid);
    TOKEN_STATISTICS stats {};
    require(GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &bytes), "Cannot query interactive-user logon generation");
    return identity + ":" + std::to_string(stats.AuthenticationId.HighPart) + ":" + std::to_string(stats.AuthenticationId.LowPart);
  }

  /**
   * @brief Capture the real console user, never the privileged Sunshine service identity.
   */
  std::shared_ptr<handle_t> interactive_token(HANDLE process, DWORD session) {
    DWORD process_session = UINT32_MAX;
    require(ProcessIdToSessionId(GetCurrentProcessId(), &process_session) && process_session == session, "Sunshine is not in the active console session");
    DWORD bytes = 0;
    GetTokenInformation(process, TokenUser, nullptr, 0, &bytes);
    require(bytes > 0 && bytes < 65536, "Cannot query Sunshine process identity");
    std::vector<unsigned char> buffer(bytes);
    require(GetTokenInformation(process, TokenUser, buffer.data(), bytes, &bytes), "Cannot query Sunshine process identity");
    auto process_user = reinterpret_cast<TOKEN_USER *>(buffer.data());
    HANDLE user = INVALID_HANDLE_VALUE;
    if (IsWellKnownSid(process_user->User.Sid, WinLocalSystemSid)) {
      impersonation_t context(process);
      require(WTSQueryUserToken(session, &user), "No active console user token");
    } else {
      DWORD shell_pid = 0;
      auto shell_window = GetShellWindow();
      require(shell_window && GetWindowThreadProcessId(shell_window, &shell_pid), "No interactive console shell");
      handle_t shell_process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell_pid));
      require(shell_process.value != nullptr && shell_process.value != INVALID_HANDLE_VALUE, "Cannot inspect interactive console shell");
      HANDLE raw_shell_token = INVALID_HANDLE_VALUE;
      require(OpenProcessToken(shell_process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &raw_shell_token), "Cannot capture interactive console user");
      handle_t shell_token(raw_shell_token);
      DWORD token_session = UINT32_MAX;
      require(GetTokenInformation(shell_token.value, TokenSessionId, &token_session, sizeof(token_session), &bytes) && token_session == session, "Interactive shell is not in the active console session");
      GetTokenInformation(shell_token.value, TokenUser, nullptr, 0, &bytes);
      require(bytes > 0 && bytes < 65536, "Cannot query interactive shell user");
      std::vector<unsigned char> shell_buffer(bytes);
      require(GetTokenInformation(shell_token.value, TokenUser, shell_buffer.data(), bytes, &bytes), "Cannot query interactive shell user");
      auto shell_user = reinterpret_cast<TOKEN_USER *>(shell_buffer.data());
      require(EqualSid(process_user->User.Sid, shell_user->User.Sid), "Sunshine process user differs from interactive console user");
      // The shell token is normally filtered even when Sunshine was elevated.
      require(DuplicateTokenEx(shell_token.value, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, nullptr, SecurityImpersonation, TokenImpersonation, &user), "Cannot duplicate interactive-user token");
    }
    return std::make_shared<handle_t>(user);
  }

  /**
   * @brief One opaque transfer, never shared across stream generations.
   */
  struct transfer_t {
    enum class state_e {
      preparing,  ///< Scanning a source or creating an incoming stage.
      ready,  ///< Source or receiver is ready for bounded batch I/O.
      verifying,  ///< Incoming batch data is being checked before publication.
      completed,  ///< Verified incoming files became pasteable.
      failed  ///< Transfer was rejected, cancelled, or could not complete.
    };  ///< Transfer lifecycle.
    std::mutex mutex;  ///< Protect lifecycle and published result.
    std::string owner;  ///< Exact TLS identity and active stream generation.
    std::string id;  ///< Random identifier independent of source/destination paths.
    uint32_t sequence = 0;  ///< Captured clipboard generation, or published generation.
    bool upload = false;  ///< Whether data flows from the client into the host.
    std::atomic<bool> cancelled {false};  ///< Abort flag sampled throughout file work.
    state_e state = state_e::preparing;  ///< Current lifecycle phase.
    std::shared_ptr<source_t> source;  ///< Immutable local file snapshot.
    std::shared_ptr<receiver_t> receiver;  ///< Private incomplete or verified receiver stage.
    std::future<void> work;  ///< Owned asynchronous scan/verify task, joined on shutdown.
    std::mutex work_mutex;  ///< Serialize future replacement and readiness probes.
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();  ///< Last network activity.
  };

  /**
   * @brief Sample cheap cancellation continuously and full pairing/user/session revocation at most every 100 ms.
   */
  cancel_t cancellation(const std::shared_ptr<transfer_t> &record, const desktop_t &desktop, const guard_t &guard, std::optional<uint32_t> expected = {}) {
    return [weak = std::weak_ptr<transfer_t>(record), desktop, guard, expected, last = std::chrono::steady_clock::time_point()]() mutable {
      auto current = weak.lock();
      if (!current || current->cancelled || (expected && desktop.sequence() != *expected)) {
        return true;
      }
      auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::milliseconds(100)) {
        last = now;
        return !guard([] {
        });
      }
      return false;
    };
  }

  /**
   * @brief Service state separate from global pairing/session locks.
   */
  class service_t::impl_t {
  public:
    std::filesystem::path storage;  ///< Fixed local stage parent.
    desktop_t desktop;  ///< Interactive desktop operations.
    std::mutex mutex;  ///< Protect transfer lookup, never held during file I/O.
    std::unordered_map<std::string, std::shared_ptr<transfer_t>> transfers;  ///< Bounded opaque registry.

    /**
     * @brief Capture fixed local storage and injectable desktop operations.
     * @param storage Private per-user stage parent.
     * @param desktop Interactive desktop and identity callbacks.
     */
    impl_t(std::filesystem::path storage, desktop_t desktop):
        storage(std::move(storage)),
        desktop(std::move(desktop)) {}

    /**
     * @brief Cancel and join workers, then remove unpublished stages as their interactive user.
     */
    ~impl_t() {
      for (auto &item : transfers) {
        item.second->cancelled = true;
      }
      for (auto &item : transfers) {
        std::lock_guard<std::mutex> lock(item.second->work_mutex);
        if (item.second->work.valid()) {
          item.second->work.wait();
        }
      }
      std::shared_ptr<void> context;
      try {
        if (desktop.enter_context) {
          context = desktop.enter_context();
        }
      } catch (...) {}
      if (desktop.enter_context && !context) {
        for (auto &item : transfers) {
          if (item.second->receiver) {
            item.second->receiver->defer_cleanup();
          }
        }
      }
      transfers.clear();  // Receiver cleanup runs under the interactive user's identity.
    }

    /**
     * @brief Retire inactive cancelled transfers without holding the registry lock during cleanup.
     */
    void prune() {
      std::vector<std::shared_ptr<transfer_t>> retired;
      {
        std::lock_guard<std::mutex> lock(mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto item = transfers.begin(); item != transfers.end();) {
          auto record = item->second;
          if (now - record->touched > std::chrono::minutes(15)) {
            record->cancelled = true;
          }
          bool finished;
          {
            std::lock_guard<std::mutex> task_lock(record->work_mutex);
            finished = !record->work.valid() || record->work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
          }
          if (record->cancelled && finished) {
            retired.push_back(record);
            item = transfers.erase(item);
          } else {
            ++item;
          }
        }
      }
      // Potentially large stage cleanup occurs outside the registry mutex.
    }

    /**
     * @brief Look up a transfer belonging to the exact authenticated stream owner.
     * @param owner Canonical certificate and stream/user generation.
     * @param id Opaque random transfer identity.
     * @return Owned transfer record, or an exception for an invalid request.
     */
    std::shared_ptr<transfer_t> find(const std::string &owner, std::string_view id) {
      require(valid_id(id), "Invalid transfer identity");
      std::lock_guard<std::mutex> lock(mutex);
      auto item = transfers.find(std::string(id));
      require(item != transfers.end() && item->second->owner == owner, "Unknown transfer identity");
      item->second->touched = std::chrono::steady_clock::now();
      return item->second;
    }

    /**
     * @brief Register a bounded transfer and supersede earlier work in the same direction.
     * @param owner Canonical certificate and stream/user generation.
     * @param sequence Captured clipboard generation.
     * @param upload Whether the remote client is sending files to this host.
     * @return New opaque transfer record.
     */
    std::shared_ptr<transfer_t> create(const std::string &owner, uint32_t sequence, bool upload) {
      auto record = std::make_shared<transfer_t>();
      record->owner = owner;
      record->sequence = sequence;
      record->upload = upload;
      record->id = random_id();
      std::lock_guard<std::mutex> lock(mutex);
      require(transfers.size() < 32, "Too many concurrent clipboard transfers");
      for (auto &item : transfers) {
        if (item.second->owner == owner && item.second->upload == upload) {
          item.second->cancelled = true;
        }
      }
      transfers.emplace(record->id, record);
      return record;
    }
  };

  service_t::service_t(std::filesystem::path storage, desktop_t desktop):
      impl(std::make_unique<impl_t>(std::move(storage), std::move(desktop))) {}

  service_t::~service_t() = default;

  response_t service_t::request(const std::string &owner, const guard_t &guard, std::string_view op, std::string_view id, uint32_t batch, uint32_t sequence, std::string_view body) {
    response_t result;
    if (!guard || !guard([] {
        })) {
      result.status = 403;
      return result;
    }
    if (body.size() > (op == "begin" ? max_manifest_bytes : max_wire_bytes)) {
      result.status = 413;
      return result;
    }
    if (owner.empty() || owner.size() > 32768) {
      return result;
    }
    try {
      auto context = impl->desktop.enter_context ? impl->desktop.enter_context() : std::shared_ptr<void>();
      require(!impl->desktop.enter_context || context != nullptr, "Cannot enter interactive-user file security context");
      if (op == "sequence") {
        require(body.empty(), "Unexpected sequence body");
        require(guard([&] {
                  result.sequence = impl->desktop.sequence();
                }),
                "Stream is no longer authorized");
        result.status = 204;
        return result;
      }
      impl->prune();
      if (op == "offer") {
        require(body.empty(), "Unexpected offer body");
        result.sequence = impl->desktop.sequence();
        if (result.sequence == sequence) {
          result.status = 204;
          return result;
        }
        std::shared_ptr<transfer_t> record;
        {
          std::lock_guard<std::mutex> lock(impl->mutex);
          for (const auto &item : impl->transfers) {
            if (item.second->owner == owner && !item.second->upload && !item.second->cancelled && item.second->sequence == result.sequence) {
              record = item.second;
              record->touched = std::chrono::steady_clock::now();
              break;
            }
          }
        }
        if (!record) {
          auto snapshot = impl->desktop.read();
          require(guard([] {
                  }),
                  "Stream is no longer authorized");
          result.sequence = snapshot.sequence;
          if (snapshot.sequence == sequence || snapshot.paths.empty()) {
            result.status = 204;
            return result;
          }
          record = impl->create(owner, snapshot.sequence, false);
          auto desktop = impl->desktop;
          std::lock_guard<std::mutex> task_lock(record->work_mutex);
          record->work = std::async(std::launch::async, [weak = std::weak_ptr<transfer_t>(record), desktop, guard, paths = std::move(snapshot.paths)] {
            auto record = weak.lock();
            if (!record) {
              return;
            }
            background_t priority;
            try {
              auto context = desktop.enter_context ? desktop.enter_context() : std::shared_ptr<void>();
              require(!desktop.enter_context || context != nullptr, "Cannot enter interactive-user file security context");
              auto cancel = cancellation(record, desktop, guard, record->sequence);
              auto source = std::make_shared<source_t>(paths, record->sequence, cancel);
              require(!cancel(), "Clipboard source changed");
              std::lock_guard<std::mutex> lock(record->mutex);
              record->source = std::move(source);
              record->state = transfer_t::state_e::ready;
            } catch (...) {
              std::lock_guard<std::mutex> lock(record->mutex);
              record->state = transfer_t::state_e::failed;
            }
          });
        }
        std::lock_guard<std::mutex> lock(record->mutex);
        result.id = record->id;
        if (record->state == transfer_t::state_e::ready) {
          result.body = encode_manifest(record->source->manifest);
          result.status = 200;
        } else {
          result.status = record->state == transfer_t::state_e::failed ? 409 : 202;
        }
        return result;
      }
      if (op == "begin") {
        auto manifest = decode_manifest(body);
        require(guard([&] {
                  result.sequence = impl->desktop.sequence();
                  require(sequence == result.sequence, "Clipboard destination changed before upload began");
                }),
                "Stream is no longer authorized");
        auto record = impl->create(owner, result.sequence, true);
        auto storage = impl->storage;
        auto desktop = impl->desktop;
        std::lock_guard<std::mutex> task_lock(record->work_mutex);
        record->work = std::async(std::launch::async, [weak = std::weak_ptr<transfer_t>(record), storage, desktop, guard, manifest = std::move(manifest)]() mutable {
          auto record = weak.lock();
          if (!record) {
            return;
          }
          background_t priority;
          try {
            auto context = desktop.enter_context ? desktop.enter_context() : std::shared_ptr<void>();
            require(!desktop.enter_context || context != nullptr, "Cannot enter interactive-user file security context");
            require(!record->cancelled && guard([] {
            }),
                    "Clipboard transfer cancelled");
            try {
              auto snapshot = desktop.read();
              prune_storage(storage, snapshot.paths, cancellation(record, desktop, guard, snapshot.sequence));
            } catch (...) {}  // Leave busy/protected/unknown stages alone.
            auto receiver = std::make_shared<receiver_t>(std::move(manifest), storage, cancellation(record, desktop, guard, record->sequence));
            require(!record->cancelled && guard([] {
            }),
                    "Clipboard transfer cancelled");
            std::lock_guard<std::mutex> lock(record->mutex);
            record->receiver = std::move(receiver);
            record->state = transfer_t::state_e::ready;
          } catch (...) {
            std::lock_guard<std::mutex> lock(record->mutex);
            record->state = transfer_t::state_e::failed;
          }
        });
        result.status = 202;
        result.id = record->id;
        result.body = record->id;
        return result;
      }
      auto record = impl->find(owner, id);
      result.id = record->id;
      if (op == "cancel") {
        require(body.empty(), "Unexpected cancel body");
        record->cancelled = true;
        result.status = 200;
        return result;
      }
      require(!record->cancelled, "Clipboard transfer cancelled");
      if (op == "status") {
        require(body.empty(), "Unexpected status body");
        std::lock_guard<std::mutex> lock(record->mutex);
        result.sequence = record->sequence;
        result.status = record->state == transfer_t::state_e::failed ? 409 : (record->state == transfer_t::state_e::preparing || record->state == transfer_t::state_e::verifying) ? 202 :
                                                                                                                                                                                    200;
        return result;
      }
      std::shared_ptr<source_t> source;
      std::shared_ptr<receiver_t> receiver;
      {
        std::lock_guard<std::mutex> lock(record->mutex);
        require(record->state == transfer_t::state_e::ready, "Clipboard transfer is not ready");
        source = record->source;
        receiver = record->receiver;
        result.sequence = record->sequence;
      }
      if (op == "batch") {
        if (record->upload) {
          receiver->accept_batch(batch, body);
          require(!record->cancelled && impl->desktop.sequence() == record->sequence && guard([] {
          }),
                  "Clipboard transfer cancelled");
        } else {
          require(body.empty(), "Unexpected download body");
          auto desktop = impl->desktop;
          result.body = source->read_batch(batch, cancellation(record, desktop, guard, record->sequence));
          require(!record->cancelled && desktop.sequence() == record->sequence && guard([] {
          }),
                  "Clipboard transfer cancelled");
        }
        result.status = 200;
        return result;
      }
      if (op == "commit") {
        require(body.empty() && record->upload && sequence == result.sequence, "Invalid clipboard commit");
        {
          std::lock_guard<std::mutex> lock(record->mutex);
          require(record->state == transfer_t::state_e::ready, "Clipboard commit already started");
          record->state = transfer_t::state_e::verifying;
        }
        // Stage preparation is already ready; waiting for its finished future is
        // bounded and avoids destroying a running task under the record lock.
        std::lock_guard<std::mutex> task_lock(record->work_mutex);
        if (record->work.valid()) {
          record->work.wait();
        }
        auto desktop = impl->desktop;
        record->work = std::async(std::launch::async, [weak = std::weak_ptr<transfer_t>(record), receiver, desktop, guard, sequence] {
          auto record = weak.lock();
          if (!record) {
            return;
          }
          background_t priority;
          try {
            auto context = desktop.enter_context ? desktop.enter_context() : std::shared_ptr<void>();
            require(!desktop.enter_context || context != nullptr, "Cannot enter interactive-user file security context");
            auto cancel = cancellation(record, desktop, guard, sequence);
            receiver->verify(cancel, desktop.enter_context);
            require(!cancel(), "Clipboard destination changed");
            std::shared_ptr<prepared_drop_t> prepared;
            if (!desktop.publish) {
              prepared = std::make_shared<prepared_drop_t>(receiver->roots);
            }
            uint32_t published = sequence;
            bool delivered = false;
            for (unsigned attempt = 0; attempt < 20 && !cancel(); attempt++) {
              try {
                require(guard([&] {
                          require(!record->cancelled && desktop.sequence() == sequence, "Clipboard destination changed");
                          published = desktop.publish ? desktop.publish(receiver->roots, sequence) : prepared->publish(sequence);
                        }),
                        "Stream is no longer authorized");
                delivered = true;
                break;
              } catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
              }
            }
            require(delivered, "Clipboard was busy or changed before file delivery");
            receiver->retain();
            std::lock_guard<std::mutex> lock(record->mutex);
            record->sequence = published;
            record->state = transfer_t::state_e::completed;
          } catch (...) {
            std::lock_guard<std::mutex> lock(record->mutex);
            record->state = transfer_t::state_e::failed;
          }
        });
        result.status = 202;
        return result;
      }
    } catch (...) {
      result.status = 409;
      result.body.clear();
    }
    return result;
  }
#endif

  response_t request(const std::string &owner, const guard_t &guard, std::string_view op, std::string_view id, uint32_t batch, uint32_t sequence, std::string_view body) {
#ifdef _WIN32
    if (!guard || !guard([] {
        })) {
      return {403, {}, 0, {}};
    }
    try {
      HANDLE raw_process = INVALID_HANDLE_VALUE;
      require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw_process), "Cannot query Sunshine process token");
      auto process = std::make_shared<handle_t>(raw_process);
      DWORD session = WTSGetActiveConsoleSessionId();
      require(session != UINT32_MAX, "No active interactive console session");
      auto user = interactive_token(process->value, session);
      auto identity = token_identity(user->value);
      const auto still_active = [process, session, identity] {
        try {
          return WTSGetActiveConsoleSessionId() == session && token_identity(interactive_token(process->value, session)->value) == identity;
        } catch (...) {
          return false;
        }
      };
      const auto bound_guard = [guard, still_active](const std::function<void()> &action) {
        return still_active() && guard(action);
      };
      wchar_t *local = nullptr;
      require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, user->value, &local)), "Cannot locate interactive-user clipboard storage");
      auto storage = std::filesystem::path(local) / L"Sunshine" / L"Clipboard";
      CoTaskMemFree(local);
      // One process can briefly overlap an old/new login. Services are separated
      // by SID and logon ID; inactive generations fail every request and publish.
      static std::mutex registry_mutex;
      static std::map<std::string, std::shared_ptr<service_t>> services;
      std::shared_ptr<service_t> service;
      std::vector<std::shared_ptr<service_t>> retired;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        for (auto entry = services.begin(); entry != services.end();) {
          if (entry->first != identity && entry->second.use_count() == 1) {
            retired.push_back(std::move(entry->second));
            entry = services.erase(entry);
          } else {
            ++entry;
          }
        }
        auto found = services.find(identity);
        if (found != services.end()) {
          service = found->second;
        } else {
          require(services.size() < 4, "Too many interactive clipboard sessions; restart Sunshine");
          desktop_t desktop;
          desktop.enter_context = [user] {
            return std::make_shared<impersonation_t>(user->value);
          };
          service = std::make_shared<service_t>(storage, std::move(desktop));
          services.emplace(identity, service);
        }
      }
      // Join and clean old user generations outside the registry lock; each
      // service destructor reinstates its original user's file permissions.
      retired.clear();
      return service->request(owner + "#" + identity, bound_guard, op, id, batch, sequence, body);
    } catch (...) {
      return {503, {}, 0, {}};
    }
#else
    return {404, {}, 0, {}};
#endif
  }
}  // namespace clipboard_files
