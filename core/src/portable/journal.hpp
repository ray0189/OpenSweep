// Persistent transaction journal for policy, recovery and config changes (Phases 5/6).
// Portable: no Windows headers. Owner: engine-act.
//
// Format: JSON Lines, append-only. Every line is one complete JSON object (a full snapshot of one
// transaction) whose last member is "crc": CRC-32 (IEEE) in 8 lower-case hex digits, computed over
// the line with the crc member removed (i.e. the object text ending in '}'). The latest valid
// snapshot of a txId wins. Corrupt lines are skipped on load and counted; if any were found the
// file is compacted. Write-ahead: begin() appends and flushes the intent record before returning,
// and callers only touch the system after begin() succeeded.
// Bounded: at most `max_records` transactions are kept (oldest finished ones are dropped); the
// file is compacted (temp + flush + rename) once it holds more than 2 * max_records lines.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace opensweep::journal {

// --- file I/O --------------------------------------------------------------------------------
class FileIO {
public:
    virtual ~FileIO() = default;
    // Appends `data` and flushes it to the OS. Returns false on any failure.
    virtual bool append(const std::string& path, std::string_view data) = 0;
    // Whole file, or nullopt when it does not exist / cannot be read.
    virtual std::optional<std::string> read(const std::string& path) = 0;
    // Atomic replace: write `path + ".tmp"`, flush, rename over `path`.
    virtual bool replace(const std::string& path, std::string_view data) = 0;
};

// std::fstream / std::filesystem implementation. Flushes the stream; it cannot fsync portably,
// so the Windows service should wrap it with FlushFileBuffers for full durability.
class StdFileIO final : public FileIO {
public:
    bool append(const std::string& path, std::string_view data) override;
    std::optional<std::string> read(const std::string& path) override;
    bool replace(const std::string& path, std::string_view data) override;
};

// In-memory implementation for tests (and dry runs).
class MemoryFileIO final : public FileIO {
public:
    bool append(const std::string& path, std::string_view data) override;
    std::optional<std::string> read(const std::string& path) override;
    bool replace(const std::string& path, std::string_view data) override;

    std::map<std::string, std::string> files;
    bool fail_append = false;
    bool fail_replace = false;
    int appends = 0;
    int replaces = 0;
};

std::uint32_t crc32(std::string_view data) noexcept;

// --- transactions ----------------------------------------------------------------------------
enum class TxState { Intent, Applied, Verifying, Retained, RolledBack, Failed, Expired };
const char* to_string(TxState s) noexcept;
std::optional<TxState> parse_state(std::string_view s) noexcept;
bool is_terminal(TxState s) noexcept;  // RolledBack, Failed, Expired

struct AuditEntry {
    std::int64_t ts = 0;
    std::string event;
    std::string detail;
};

struct Transaction {
    std::string tx_id;                 // unique; assigned by begin() when empty
    std::string kind = "policy";       // "policy" | "recovery" | "config"
    std::string trigger;
    std::vector<std::string> evidence;
    std::vector<std::string> preconditions;
    std::uint32_t target_pid = 0;
    std::string target;                // name of the process / service / component
    std::int64_t target_start_ms = 0;  // PID-reuse guard
    std::string action;                // e.g. "ecoqos", "restart_collector", "set_mode"
    std::int64_t value = 0;
    std::int64_t original = 0;
    bool has_original = false;
    std::string required_auth = "none";  // "none" | "user" | "admin"
    TxState state = TxState::Intent;
    std::string verification;          // procedure
    std::string verification_result;
    std::string rollback;              // procedure
    std::int64_t cooldown_until = 0;
    int attempt = 1;
    int max_attempts = 1;
    std::int64_t created_ms = 0;
    std::int64_t updated_ms = 0;
    std::vector<AuditEntry> audit;
};

json::Value to_json(const Transaction& t);
std::optional<Transaction> from_json(const json::Value& v);

struct JournalOptions {
    std::size_t max_records = 5000;
    std::size_t max_audit = 16;      // per transaction (first entry always kept)
    std::size_t max_evidence = 16;   // items per list
    std::size_t max_string = 256;    // bytes per free-text field
    std::size_t max_line = 64 * 1024;
};

struct LoadReport {
    std::size_t lines = 0;
    std::size_t loaded = 0;   // distinct transactions after load
    std::size_t corrupt = 0;  // skipped lines
    bool compacted = false;
};

class Journal {
public:
    Journal(FileIO& io, std::string path, JournalOptions opts = {});

    LoadReport load();

    // Write-ahead intent: assigns tx_id (if empty), sets state Intent and timestamps, appends +
    // flushes. Returns the id, or nullopt if it could not be persisted (caller must not act).
    std::optional<std::string> begin(Transaction tx, std::int64_t now_ms);

    // Changes the state (illegal transitions out of a terminal state are refused), runs `mut`
    // (may be empty), appends an audit entry and persists. Returns false if unknown/refused/IO error
    // (the in-memory state is still updated on an IO error so the engine stays consistent).
    bool update(const std::string& tx_id, std::int64_t now_ms, TxState state, std::string_view event,
                std::string_view detail, const std::function<void(Transaction&)>& mut = {});

    const Transaction* find(const std::string& tx_id) const;
    // Transactions in intent / applied / verifying (+ retained when include_retained), oldest first.
    std::vector<const Transaction*> unfinished(bool include_retained = false) const;
    // Newest first, at most `limit`.
    std::vector<const Transaction*> recent(std::size_t limit) const;

    std::size_t size() const noexcept { return by_seq_.size(); }
    std::size_t file_lines() const noexcept { return file_lines_; }
    std::size_t corrupt_lines() const noexcept { return corrupt_; }
    std::size_t io_errors() const noexcept { return io_errors_; }
    const std::string& path() const noexcept { return path_; }

    // Serialises one line (with crc). Exposed for tests.
    static std::string encode_line(const Transaction& t);
    static std::optional<Transaction> decode_line(std::string_view line);

private:
    bool persist(const Transaction& t);
    void sanitize(Transaction& t) const;
    void enforce_bound();
    bool compact();
    std::string new_id(std::int64_t now_ms);

    FileIO& io_;
    std::string path_;
    JournalOptions opts_;
    std::map<std::uint64_t, Transaction> by_seq_;  // creation order
    std::unordered_map<std::string, std::uint64_t> index_;
    std::uint64_t next_seq_ = 1;
    std::uint64_t id_counter_ = 0;
    std::size_t file_lines_ = 0;
    std::size_t corrupt_ = 0;
    std::size_t io_errors_ = 0;
};

}  // namespace opensweep::journal
