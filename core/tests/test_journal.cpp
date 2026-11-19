#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include "journal.hpp"
#include "test_framework.hpp"

using namespace opensweep::journal;

namespace {

Transaction sample_tx(const char* target = "onedrive.exe") {
    Transaction t;
    t.kind = "policy";
    t.trigger = "contention";
    t.evidence = {"cpu 95%", "queue 2.1"};
    t.preconditions = {"alive"};
    t.target_pid = 42;
    t.target = target;
    t.target_start_ms = 1234;
    t.action = "ecoqos";
    t.value = 1;
    t.required_auth = "user";
    t.verification = "compare";
    t.rollback = "restore";
    t.cooldown_until = 99;
    return t;
}

std::size_t count_lines(const std::string& s) {
    std::size_t n = 0;
    for (char c : s)
        if (c == '\n') ++n;
    return n;
}

}  // namespace

TEST(journal_crc32_known_vector) {
    CHECK_EQ(crc32("123456789"), 0xCBF43926u);
    CHECK_EQ(crc32(""), 0u);
}

TEST(journal_line_roundtrip_and_crc) {
    Transaction t = sample_tx();
    t.tx_id = "tx-1";
    t.original = -1;
    t.has_original = true;
    t.audit.push_back({5, "intent", "x"});
    const std::string line = Journal::encode_line(t);
    CHECK(line.back() == '\n');
    const std::string body = line.substr(0, line.size() - 1);
    CHECK(opensweep::json::parse(body).ok());  // every line is valid JSON
    auto back = Journal::decode_line(body);
    CHECK(back.has_value());
    CHECK_EQ(back->tx_id, std::string("tx-1"));
    CHECK_EQ(back->target_pid, 42u);
    CHECK_EQ(back->target_start_ms, 1234);
    CHECK_EQ(back->original, -1);
    CHECK(back->has_original);
    CHECK_EQ(back->evidence.size(), 2u);
    CHECK_EQ(back->audit.size(), 1u);
    CHECK(back->state == TxState::Intent);
    // flip one byte inside the payload -> crc mismatch
    std::string bad = body;
    bad[10] = bad[10] == 'a' ? 'b' : 'a';
    CHECK(!Journal::decode_line(bad).has_value());
    CHECK(!Journal::decode_line("garbage").has_value());
    CHECK(!Journal::decode_line("{\"txId\":\"x\",\"crc\":\"zzzzzzzz\"}").has_value());
    // json shape validated even with a correct crc
    Transaction no_kind = t;
    no_kind.kind = "bogus";
    CHECK(!Journal::decode_line(Journal::encode_line(no_kind).substr(0)).has_value());
}

TEST(journal_begin_writes_intent_first) {
    MemoryFileIO io;
    Journal j(io, "j.jsonl");
    auto id = j.begin(sample_tx(), 1000);
    CHECK(id.has_value());
    CHECK_EQ(io.appends, 1);
    const std::string& file = io.files["j.jsonl"];
    CHECK(file.find("\"state\":\"intent\"") != std::string::npos);
    CHECK(file.find(*id) != std::string::npos);
    const Transaction* t = j.find(*id);
    CHECK(t != nullptr);
    CHECK(t->state == TxState::Intent);
    CHECK_EQ(t->audit.size(), 1u);
    CHECK_EQ(t->created_ms, 1000);
}

TEST(journal_begin_fails_closed) {
    MemoryFileIO io;
    io.fail_append = true;
    Journal j(io, "j.jsonl");
    CHECK(!j.begin(sample_tx(), 1).has_value());
    CHECK_EQ(j.size(), 0u);
    CHECK_EQ(j.io_errors(), 1u);
}

TEST(journal_unique_ids) {
    MemoryFileIO io;
    Journal j(io, "j.jsonl");
    std::set<std::string> ids;
    for (int i = 0; i < 200; ++i) ids.insert(*j.begin(sample_tx(), 7));  // same timestamp
    CHECK_EQ(ids.size(), 200u);
    Transaction dup = sample_tx();
    dup.tx_id = *ids.begin();
    auto other = j.begin(dup, 7);
    CHECK(other.has_value() && *other != *ids.begin());  // a colliding id is replaced
}

TEST(journal_state_machine) {
    MemoryFileIO io;
    Journal j(io, "j.jsonl");
    auto id = *j.begin(sample_tx(), 1);
    CHECK(j.update(id, 2, TxState::Applied, "applied", "original=0", [](Transaction& t) {
        t.original = 0;
        t.has_original = true;
        t.state = TxState::Failed;  // ignored: update() owns the state
    }));
    CHECK(j.find(id)->state == TxState::Applied);
    CHECK(!j.update(id, 3, TxState::Intent, "x", "y"));  // cannot go back to intent
    CHECK(j.update(id, 3, TxState::Verifying, "verifying", ""));
    CHECK(j.update(id, 4, TxState::Retained, "verified", ""));
    CHECK(j.update(id, 5, TxState::RolledBack, "rolled_back", "retention"));
    CHECK(!j.update(id, 6, TxState::Applied, "x", ""));  // terminal
    CHECK(!j.update("nope", 6, TxState::Applied, "x", ""));
    CHECK_EQ(j.find(id)->audit.size(), 5u);
    CHECK_EQ(j.find(id)->updated_ms, 5);
}

TEST(journal_unfinished_and_reload) {
    MemoryFileIO io;
    std::string a, b, c, d;
    {
        Journal j(io, "j.jsonl");
        a = *j.begin(sample_tx("a"), 1);                         // intent
        b = *j.begin(sample_tx("b"), 2);
        j.update(b, 2, TxState::Applied, "applied", "");          // applied
        c = *j.begin(sample_tx("c"), 3);
        j.update(c, 3, TxState::Verifying, "verifying", "");      // verifying
        j.update(c, 4, TxState::Retained, "verified", "");        // retained
        d = *j.begin(sample_tx("d"), 5);
        j.update(d, 5, TxState::RolledBack, "rolled_back", "");  // finished
    }
    Journal j2(io, "j.jsonl");
    const LoadReport r = j2.load();
    CHECK_EQ(r.corrupt, 0u);
    CHECK_EQ(r.loaded, 4u);
    CHECK_EQ(r.lines, 8u);  // a:1 b:2 c:3 d:2
    auto u = j2.unfinished();
    CHECK_EQ(u.size(), 2u);
    CHECK_EQ(u[0]->tx_id, a);
    CHECK_EQ(u[1]->tx_id, b);
    CHECK_EQ(j2.unfinished(true).size(), 3u);
    auto recent = j2.recent(2);
    CHECK_EQ(recent.size(), 2u);
    CHECK_EQ(recent[0]->tx_id, d);  // newest first
    // ids keep being unique after reload
    auto e = *j2.begin(sample_tx("e"), 1);
    CHECK(e != a && e != b && e != c && e != d);
}

TEST(journal_skips_corrupt_lines) {
    MemoryFileIO io;
    {
        Journal j(io, "j.jsonl");
        for (int i = 0; i < 5; ++i) j.begin(sample_tx(), i);
    }
    std::string& f = io.files["j.jsonl"];
    // corrupt the 2nd line (bit flip), inject garbage and a torn final write
    const auto first_nl = f.find('\n');
    f[first_nl + 20] ^= 0x01;
    f.insert(0, "not json at all\n");
    f += "{\"txId\":\"torn";
    Journal j2(io, "j.jsonl");
    const LoadReport r = j2.load();
    CHECK_EQ(r.corrupt, 3u);
    CHECK_EQ(r.loaded, 4u);
    CHECK(r.compacted);
    CHECK_EQ(j2.corrupt_lines(), 3u);
    // compacted file is clean
    Journal j3(io, "j.jsonl");
    const LoadReport r3 = j3.load();
    CHECK_EQ(r3.corrupt, 0u);
    CHECK_EQ(r3.loaded, 4u);
    CHECK_EQ(count_lines(io.files["j.jsonl"]), 4u);
}

TEST(journal_rotation_bound) {
    MemoryFileIO io;
    JournalOptions opts;
    opts.max_records = 10;
    Journal j(io, "j.jsonl", opts);
    std::string keep;
    for (int i = 0; i < 200; ++i) {
        auto id = *j.begin(sample_tx(), i);
        if (i == 3) {
            keep = id;  // left applied: must survive rotation (needs rollback)
            j.update(id, i, TxState::Applied, "applied", "");
            continue;
        }
        j.update(id, i, TxState::Applied, "applied", "");
        j.update(id, i, TxState::RolledBack, "rolled_back", "");
        CHECK(j.size() <= 10u);
        CHECK(j.file_lines() <= 20u);
        CHECK(count_lines(io.files["j.jsonl"]) <= 20u);
    }
    CHECK(io.replaces > 0);
    CHECK(j.find(keep) != nullptr);
    Journal j2(io, "j.jsonl", opts);
    const LoadReport r = j2.load();
    CHECK(r.loaded <= 10u);
    CHECK_EQ(r.corrupt, 0u);
    CHECK(j2.find(keep) != nullptr);
    CHECK_EQ(j2.unfinished().size(), 1u);
}

TEST(journal_bounds_audit_and_strings) {
    MemoryFileIO io;
    JournalOptions opts;
    opts.max_audit = 4;
    Journal j(io, "j.jsonl", opts);
    Transaction t = sample_tx();
    t.trigger = std::string(5000, 'x');
    for (int i = 0; i < 100; ++i) t.evidence.push_back("e");
    auto id = *j.begin(t, 1);
    for (int i = 0; i < 20; ++i) j.update(id, 2 + i, TxState::Verifying, "tick", std::to_string(i));
    const Transaction* got = j.find(id);
    CHECK_EQ(got->audit.size(), 4u);
    CHECK_EQ(got->audit.front().event, std::string("intent"));  // first entry kept
    CHECK_EQ(got->audit.back().detail, std::string("19"));
    CHECK(got->trigger.size() <= 256u);
    CHECK(got->evidence.size() <= 16u);
}

TEST(journal_std_file_io) {
    const auto dir = std::filesystem::temp_directory_path() / "opensweep_journal_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "journal.jsonl").string();
    StdFileIO io;
    CHECK(!io.read(path).has_value());
    std::string id;
    {
        Journal j(io, path);
        CHECK_EQ(j.load().loaded, 0u);
        id = *j.begin(sample_tx(), 10);
        CHECK(j.update(id, 11, TxState::Applied, "applied", ""));
    }
    {
        Journal j(io, path);
        const LoadReport r = j.load();
        CHECK_EQ(r.loaded, 1u);
        CHECK(j.find(id) && j.find(id)->state == TxState::Applied);
    }
    CHECK(io.replace(path, "x\n"));
    const auto replaced = io.read(path);
    CHECK(replaced.has_value() && *replaced == "x\n");
    CHECK(!std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove_all(dir, ec);
}
