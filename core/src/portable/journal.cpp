#include "journal.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace opensweep::journal {

namespace {

std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();

constexpr std::string_view kCrcPrefix = ",\"crc\":\"";
constexpr std::size_t kCrcSuffixLen = 8 + 8 + 2;  // ,"crc":" + 8 hex + "}

json::Value strings_to_json(const std::vector<std::string>& v) {
    json::Array a;
    a.reserve(v.size());
    for (const auto& s : v) a.emplace_back(s);
    return json::Value(std::move(a));
}

bool strings_from_json(const json::Value* v, std::vector<std::string>& out) {
    if (!v) return true;
    if (!v->is_array()) return false;
    for (const auto& e : v->as_array()) {
        if (!e.is_string()) return false;
        out.push_back(e.as_string());
    }
    return true;
}

std::int64_t get_int(const json::Value& o, std::string_view key, std::int64_t def = 0) {
    const json::Value* v = o.find(key);
    if (!v) return def;
    auto i = v->as_int();
    return i ? *i : def;
}

std::string get_str(const json::Value& o, std::string_view key) {
    const json::Value* v = o.find(key);
    return (v && v->is_string()) ? v->as_string() : std::string();
}

void truncate(std::string& s, std::size_t max) {
    if (s.size() <= max) return;
    std::size_t n = max;
    // do not cut a UTF-8 sequence in half
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
    s.resize(n);
}

}  // namespace

// --- I/O -------------------------------------------------------------------------------------
bool StdFileIO::append(const std::string& path, std::string_view data) {
    std::ofstream f(to_path(path), std::ios::binary | std::ios::app);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    return static_cast<bool>(f);
}

std::optional<std::string> StdFileIO::read(const std::string& path) {
    std::ifstream f(to_path(path), std::ios::binary);
    if (!f) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return std::nullopt;
    return s;
}

bool StdFileIO::replace(const std::string& path, std::string_view data) {
    const auto target = to_path(path);
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool MemoryFileIO::append(const std::string& path, std::string_view data) {
    ++appends;
    if (fail_append) return false;
    files[path].append(data);
    return true;
}

std::optional<std::string> MemoryFileIO::read(const std::string& path) {
    auto it = files.find(path);
    if (it == files.end()) return std::nullopt;
    return it->second;
}

bool MemoryFileIO::replace(const std::string& path, std::string_view data) {
    ++replaces;
    if (fail_replace) return false;
    files[path] = std::string(data);
    return true;
}

std::uint32_t crc32(std::string_view data) noexcept {
    std::uint32_t c = 0xFFFFFFFFu;
    for (char ch : data) c = kCrcTable[(c ^ static_cast<unsigned char>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// --- states ----------------------------------------------------------------------------------
const char* to_string(TxState s) noexcept {
    switch (s) {
        case TxState::Intent: return "intent";
        case TxState::Applied: return "applied";
        case TxState::Verifying: return "verifying";
        case TxState::Retained: return "retained";
        case TxState::RolledBack: return "rolled_back";
        case TxState::Failed: return "failed";
        case TxState::Expired: return "expired";
    }
    return "failed";
}

std::optional<TxState> parse_state(std::string_view s) noexcept {
    for (TxState st : {TxState::Intent, TxState::Applied, TxState::Verifying, TxState::Retained, TxState::RolledBack,
                       TxState::Failed, TxState::Expired})
        if (s == to_string(st)) return st;
    return std::nullopt;
}

bool is_terminal(TxState s) noexcept {
    return s == TxState::RolledBack || s == TxState::Failed || s == TxState::Expired;
}

// --- JSON ------------------------------------------------------------------------------------
json::Value to_json(const Transaction& t) {
    json::Value o = json::Value::object();
    o.set("txId", t.tx_id);
    o.set("kind", t.kind);
    o.set("trigger", t.trigger);
    o.set("evidence", strings_to_json(t.evidence));
    o.set("preconditions", strings_to_json(t.preconditions));
    json::Value target = json::Value::object();
    target.set("pid", t.target_pid);
    target.set("name", t.target);
    target.set("startMs", t.target_start_ms);
    o.set("target", std::move(target));
    o.set("action", t.action);
    o.set("value", t.value);
    if (t.has_original)
        o.set("original", t.original);
    else
        o.set("original", nullptr);
    o.set("requiredAuth", t.required_auth);
    o.set("state", to_string(t.state));
    o.set("verification", t.verification);
    o.set("verificationResult", t.verification_result);
    o.set("rollback", t.rollback);
    o.set("cooldownUntil", t.cooldown_until);
    o.set("attempt", t.attempt);
    o.set("maxAttempts", t.max_attempts);
    o.set("createdMs", t.created_ms);
    o.set("updatedMs", t.updated_ms);
    json::Array audit;
    audit.reserve(t.audit.size());
    for (const auto& a : t.audit) {
        json::Value e = json::Value::object();
        e.set("ts", a.ts);
        e.set("event", a.event);
        e.set("detail", a.detail);
        audit.push_back(std::move(e));
    }
    o.set("audit", json::Value(std::move(audit)));
    return o;
}

std::optional<Transaction> from_json(const json::Value& v) {
    if (!v.is_object()) return std::nullopt;
    Transaction t;
    t.tx_id = get_str(v, "txId");
    if (t.tx_id.empty()) return std::nullopt;
    const auto st = parse_state(get_str(v, "state"));
    if (!st) return std::nullopt;
    t.state = *st;
    t.kind = get_str(v, "kind");
    if (t.kind != "policy" && t.kind != "recovery" && t.kind != "config") return std::nullopt;
    t.trigger = get_str(v, "trigger");
    if (!strings_from_json(v.find("evidence"), t.evidence)) return std::nullopt;
    if (!strings_from_json(v.find("preconditions"), t.preconditions)) return std::nullopt;
    if (const json::Value* tg = v.find("target"); tg && tg->is_object()) {
        const std::int64_t pid = get_int(*tg, "pid");
        if (pid < 0 || pid > 0xFFFFFFFFll) return std::nullopt;
        t.target_pid = static_cast<std::uint32_t>(pid);
        t.target = get_str(*tg, "name");
        t.target_start_ms = get_int(*tg, "startMs");
    }
    t.action = get_str(v, "action");
    t.value = get_int(v, "value");
    if (const json::Value* o = v.find("original"); o && o->as_int()) {
        t.original = *o->as_int();
        t.has_original = true;
    }
    t.required_auth = get_str(v, "requiredAuth");
    if (t.required_auth.empty()) t.required_auth = "none";
    t.verification = get_str(v, "verification");
    t.verification_result = get_str(v, "verificationResult");
    t.rollback = get_str(v, "rollback");
    t.cooldown_until = get_int(v, "cooldownUntil");
    t.attempt = static_cast<int>(get_int(v, "attempt", 1));
    t.max_attempts = static_cast<int>(get_int(v, "maxAttempts", 1));
    t.created_ms = get_int(v, "createdMs");
    t.updated_ms = get_int(v, "updatedMs");
    if (const json::Value* a = v.find("audit"); a && a->is_array()) {
        for (const auto& e : a->as_array()) {
            if (!e.is_object()) return std::nullopt;
            t.audit.push_back({get_int(e, "ts"), get_str(e, "event"), get_str(e, "detail")});
        }
    }
    return t;
}

// --- Journal ---------------------------------------------------------------------------------
Journal::Journal(FileIO& io, std::string path, JournalOptions opts)
    : io_(io), path_(std::move(path)), opts_(opts) {
    if (opts_.max_records == 0) opts_.max_records = 1;
    if (opts_.max_audit < 2) opts_.max_audit = 2;
}

std::string Journal::encode_line(const Transaction& t) {
    std::string s = json::serialize(to_json(t));
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>(crc32(s)));
    s.pop_back();  // '}'
    s += kCrcPrefix;
    s += hex;
    s += "\"}\n";
    return s;
}

std::optional<Transaction> Journal::decode_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() <= kCrcSuffixLen + 2) return std::nullopt;
    const std::size_t at = line.size() - kCrcSuffixLen;
    if (line.substr(at, kCrcPrefix.size()) != kCrcPrefix || line.substr(line.size() - 2) != "\"}")
        return std::nullopt;
    const std::string_view hex = line.substr(at + kCrcPrefix.size(), 8);
    std::uint32_t want = 0;
    for (char c : hex) {
        std::uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
        else return std::nullopt;
        want = (want << 4) | d;
    }
    std::string payload(line.substr(0, at));
    payload.push_back('}');
    if (crc32(payload) != want) return std::nullopt;
    auto parsed = json::parse(payload);
    if (!parsed.ok()) return std::nullopt;
    return from_json(*parsed.value);
}

LoadReport Journal::load() {
    LoadReport r;
    by_seq_.clear();
    index_.clear();
    file_lines_ = 0;
    corrupt_ = 0;
    const auto data = io_.read(path_);
    if (!data) return r;
    std::string_view rest(*data);
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        if (line.empty() || (line.size() == 1 && line[0] == '\r')) continue;
        ++r.lines;
        std::optional<Transaction> t;
        if (line.size() <= opts_.max_line && nl != std::string_view::npos) t = decode_line(line);
        if (!t) {
            ++r.corrupt;
            continue;
        }
        sanitize(*t);
        auto it = index_.find(t->tx_id);
        if (it != index_.end()) {
            by_seq_[it->second] = std::move(*t);
        } else {
            const std::uint64_t seq = next_seq_++;
            index_.emplace(t->tx_id, seq);
            by_seq_.emplace(seq, std::move(*t));
        }
    }
    file_lines_ = r.lines;
    corrupt_ = r.corrupt;
    enforce_bound();
    if (r.corrupt > 0 || file_lines_ > 2 * opts_.max_records) r.compacted = compact();
    r.loaded = by_seq_.size();
    return r;
}

void Journal::sanitize(Transaction& t) const {
    auto cap_list = [&](std::vector<std::string>& v) {
        if (v.size() > opts_.max_evidence) v.resize(opts_.max_evidence);
        for (auto& s : v) truncate(s, opts_.max_string);
    };
    cap_list(t.evidence);
    cap_list(t.preconditions);
    truncate(t.trigger, opts_.max_string);
    truncate(t.target, opts_.max_string);
    truncate(t.action, opts_.max_string);
    truncate(t.verification, opts_.max_string);
    truncate(t.verification_result, opts_.max_string);
    truncate(t.rollback, opts_.max_string);
    if (t.audit.size() > opts_.max_audit) {
        // keep the first entry (intent) and the most recent ones
        const std::size_t drop = t.audit.size() - opts_.max_audit;
        t.audit.erase(t.audit.begin() + 1, t.audit.begin() + 1 + static_cast<std::ptrdiff_t>(drop));
    }
    for (auto& a : t.audit) {
        truncate(a.event, 64);
        truncate(a.detail, opts_.max_string);
    }
}

std::string Journal::new_id(std::int64_t now_ms) {
    for (;;) {
        std::string id = "tx-" + std::to_string(now_ms) + "-" + std::to_string(++id_counter_);
        if (index_.find(id) == index_.end()) return id;
    }
}

bool Journal::persist(const Transaction& t) {
    if (!io_.append(path_, encode_line(t))) {
        ++io_errors_;
        return false;
    }
    ++file_lines_;
    if (file_lines_ > 2 * opts_.max_records) compact();
    return true;
}

std::optional<std::string> Journal::begin(Transaction tx, std::int64_t now_ms) {
    if (tx.tx_id.empty() || index_.find(tx.tx_id) != index_.end()) tx.tx_id = new_id(now_ms);
    tx.state = TxState::Intent;
    tx.created_ms = now_ms;
    tx.updated_ms = now_ms;
    tx.audit.clear();
    tx.audit.push_back({now_ms, "intent", tx.trigger});
    sanitize(tx);
    if (!persist(tx)) return std::nullopt;  // write-ahead failed: caller must not act
    const std::uint64_t seq = next_seq_++;
    index_.emplace(tx.tx_id, seq);
    std::string id = tx.tx_id;
    by_seq_.emplace(seq, std::move(tx));
    enforce_bound();
    return id;
}

bool Journal::update(const std::string& tx_id, std::int64_t now_ms, TxState state, std::string_view event,
                     std::string_view detail, const std::function<void(Transaction&)>& mut) {
    auto it = index_.find(tx_id);
    if (it == index_.end()) return false;
    Transaction& t = by_seq_.at(it->second);
    if (is_terminal(t.state)) return false;
    if (state == TxState::Intent && t.state != TxState::Intent) return false;
    t.state = state;
    if (mut) mut(t);
    t.state = state;  // the mutator may not change the state behind our back
    t.updated_ms = now_ms;
    t.audit.push_back({now_ms, std::string(event), std::string(detail)});
    sanitize(t);
    return persist(t);
}

const Transaction* Journal::find(const std::string& tx_id) const {
    auto it = index_.find(tx_id);
    return it == index_.end() ? nullptr : &by_seq_.at(it->second);
}

std::vector<const Transaction*> Journal::unfinished(bool include_retained) const {
    std::vector<const Transaction*> out;
    for (const auto& [seq, t] : by_seq_) {
        if (t.state == TxState::Intent || t.state == TxState::Applied || t.state == TxState::Verifying ||
            (include_retained && t.state == TxState::Retained))
            out.push_back(&t);
    }
    return out;
}

std::vector<const Transaction*> Journal::recent(std::size_t limit) const {
    std::vector<const Transaction*> out;
    for (auto it = by_seq_.rbegin(); it != by_seq_.rend() && out.size() < limit; ++it) out.push_back(&it->second);
    return out;
}

void Journal::enforce_bound() {
    auto it = by_seq_.begin();
    while (by_seq_.size() > opts_.max_records && it != by_seq_.end()) {
        const Transaction& t = it->second;
        const bool droppable = is_terminal(t.state) || (t.state == TxState::Retained && t.kind != "policy");
        if (droppable) {
            index_.erase(t.tx_id);
            it = by_seq_.erase(it);
        } else {
            ++it;  // never drop a record that may still need a rollback
        }
    }
}

bool Journal::compact() {
    std::string out;
    for (const auto& [seq, t] : by_seq_) out += encode_line(t);
    if (!io_.replace(path_, out)) {
        ++io_errors_;
        return false;
    }
    file_lines_ = by_seq_.size();
    return true;
}

}  // namespace opensweep::journal
