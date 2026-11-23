#include "HistoryStore.h"

#include <windows.h>
#include <winsqlite/winsqlite3.h>

#include <memory>
#include <string>
#include <string_view>

#pragma comment(lib, "winsqlite3.lib")

namespace yiyi {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

constexpr auto selectRecord = "SELECT id,input,output,kind,dictionary,source,target,provider,model,created,favorite FROM records";

void checkSqlite(sqlite3* db, int result, int expected = SQLITE_OK)
{
    if (result == expected) return;
    auto const detail = winrt::to_hstring(db ? sqlite3_errmsg(db) : sqlite3_errstr(result));
    throw winrt::hresult_error(E_FAIL, winrt::hstring{L"历史记录数据库操作失败。" + std::wstring{detail}});
}

Statement prepare(sqlite3* db, char const* sql)
{
    sqlite3_stmt* statement{};
    checkSqlite(db, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr));
    return Statement{statement, sqlite3_finalize};
}

void bindText(sqlite3* db, Statement const& statement, int index, winrt::hstring const& value)
{
    auto const text = winrt::to_string(value);
    checkSqlite(db, sqlite3_bind_text64(statement.get(), index, text.c_str(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
}

winrt::hstring readText(sqlite3_stmt* statement, int column)
{
    auto const bytes = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
    auto const length = static_cast<std::size_t>(sqlite3_column_bytes(statement, column));
    return bytes ? winrt::to_hstring(std::string_view{bytes, length}) : winrt::hstring{};
}

std::wstring lowerForSearch(std::wstring_view text)
{
    if (text.empty()) return {};
    auto const length = static_cast<int>(text.size());
    auto const required = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), length,
                                        nullptr, 0, nullptr, nullptr, 0);
    if (required == 0) winrt::throw_last_error();
    std::wstring lower(required, L'\0');
    auto const written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), length,
                                       lower.data(), required, nullptr, nullptr, 0);
    if (written == 0) winrt::throw_last_error();
    lower.resize(written);
    return lower;
}

bool containsSearch(sqlite3_stmt* statement, int column, std::wstring_view lowerSearch)
{
    auto const* text = static_cast<wchar_t const*>(sqlite3_column_text16(statement, column));
    if (!text) return false;
    auto const length = static_cast<std::size_t>(sqlite3_column_bytes16(statement, column)) / sizeof(wchar_t);
    return lowerForSearch({text, length}).find(lowerSearch) != std::wstring::npos;
}

HistoryRecord readRecord(sqlite3_stmt* statement)
{
    HistoryRecord record;
    record.id = sqlite3_column_int64(statement, 0);
    record.input = readText(statement, 1);
    record.output = readText(statement, 2);
    record.kind = readText(statement, 3);
    record.dictionary = readText(statement, 4);
    record.source = readText(statement, 5);
    record.target = readText(statement, 6);
    record.provider = readText(statement, 7);
    record.model = readText(statement, 8);
    record.created = readText(statement, 9);
    record.favorite = sqlite3_column_int(statement, 10) != 0;
    return record;
}

}

HistoryStore::HistoryStore(std::filesystem::path const& directory)
{
    std::filesystem::create_directories(directory);
    auto const path = winrt::to_string((directory / L"history.sqlite").native());
    sqlite3* handle{};
    auto const result = sqlite3_open_v2(path.c_str(), &handle,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    auto db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{handle, sqlite3_close};
    checkSqlite(handle, result);
    checkSqlite(handle, sqlite3_busy_timeout(handle, 3000));
    checkSqlite(handle, sqlite3_exec(handle, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr));
    checkSqlite(handle, sqlite3_exec(handle,
        "CREATE TABLE IF NOT EXISTS records ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, input TEXT NOT NULL, output TEXT NOT NULL,"
        "kind TEXT NOT NULL, dictionary TEXT NOT NULL DEFAULT '{}',"
        "source TEXT NOT NULL, target TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL,"
        "created TEXT NOT NULL, favorite INTEGER NOT NULL DEFAULT 0)", nullptr, nullptr, nullptr));
    m_db = db.release();
}

HistoryStore::~HistoryStore()
{
    sqlite3_close(m_db);
}

std::int64_t HistoryStore::add(HistoryRecord const& record)
{
    std::lock_guard lock{m_mutex};
    auto const statement = prepare(m_db,
        "INSERT INTO records (input,output,kind,dictionary,source,target,provider,model,created,favorite) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,strftime('%Y-%m-%dT%H:%M:%fZ','now'),?9)");
    bindText(m_db, statement, 1, record.input);
    bindText(m_db, statement, 2, record.output);
    bindText(m_db, statement, 3, record.kind);
    bindText(m_db, statement, 4, record.dictionary);
    bindText(m_db, statement, 5, record.source);
    bindText(m_db, statement, 6, record.target);
    bindText(m_db, statement, 7, record.provider);
    bindText(m_db, statement, 8, record.model);
    checkSqlite(m_db, sqlite3_bind_int(statement.get(), 9, record.favorite));
    checkSqlite(m_db, sqlite3_step(statement.get()), SQLITE_DONE);
    return sqlite3_last_insert_rowid(m_db);
}

std::vector<HistoryRecord> HistoryStore::list(bool favoritesOnly, winrt::hstring const& search) const
{
    std::lock_guard lock{m_mutex};
    auto const query = lowerForSearch(search);
    auto const sql = std::string{selectRecord} + (favoritesOnly ? " WHERE favorite=1" : "") + " ORDER BY id DESC";
    auto const statement = prepare(m_db, sql.c_str());
    std::vector<HistoryRecord> records;
    auto result = sqlite3_step(statement.get());
    while (result == SQLITE_ROW) {
        if (query.empty() || containsSearch(statement.get(), 1, query) || containsSearch(statement.get(), 2, query)) {
            records.push_back(readRecord(statement.get()));
        }
        result = sqlite3_step(statement.get());
    }
    checkSqlite(m_db, result, SQLITE_DONE);
    return records;
}

std::size_t HistoryStore::count(bool favoritesOnly) const
{
    std::lock_guard lock{m_mutex};
    auto const statement = prepare(m_db, favoritesOnly ? "SELECT COUNT(*) FROM records WHERE favorite=1" : "SELECT COUNT(*) FROM records");
    checkSqlite(m_db, sqlite3_step(statement.get()), SQLITE_ROW);
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

std::optional<HistoryRecord> HistoryStore::get(std::int64_t id) const
{
    std::lock_guard lock{m_mutex};
    auto const sql = std::string{selectRecord} + " WHERE id=?1";
    auto const statement = prepare(m_db, sql.c_str());
    checkSqlite(m_db, sqlite3_bind_int64(statement.get(), 1, id));
    auto const result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    checkSqlite(m_db, result, SQLITE_ROW);
    return readRecord(statement.get());
}

bool HistoryStore::setFavorite(std::int64_t id, bool favorite)
{
    std::lock_guard lock{m_mutex};
    auto const statement = prepare(m_db, "UPDATE records SET favorite=?1 WHERE id=?2");
    checkSqlite(m_db, sqlite3_bind_int(statement.get(), 1, favorite));
    checkSqlite(m_db, sqlite3_bind_int64(statement.get(), 2, id));
    checkSqlite(m_db, sqlite3_step(statement.get()), SQLITE_DONE);
    return sqlite3_changes(m_db) != 0;
}

bool HistoryStore::remove(std::int64_t id)
{
    std::lock_guard lock{m_mutex};
    auto const statement = prepare(m_db, "DELETE FROM records WHERE id=?1");
    checkSqlite(m_db, sqlite3_bind_int64(statement.get(), 1, id));
    checkSqlite(m_db, sqlite3_step(statement.get()), SQLITE_DONE);
    return sqlite3_changes(m_db) != 0;
}

}
