#include "pdftaskstore.h"

#include <windows.h>
#undef GetObject
#include <winsqlite/winsqlite3.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <regex>

namespace yiyi::pdf {
namespace {

using namespace winrt;
using namespace Windows::Data::Json;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

[[noreturn]] void invalid(wchar_t const* message)
{
    throw hresult_error(E_INVALIDARG, message);
}

void sqlCheck(sqlite3* db, int actual, int expected = SQLITE_OK)
{
    if (actual == expected) return;
    const auto detail = to_hstring(db ? sqlite3_errmsg(db) : sqlite3_errstr(actual));
    throw hresult_error(E_FAIL, hstring(L"PDF 任务数据读写失败：" + std::wstring(detail)));
}

void execute(sqlite3* db, char const* sql)
{
    sqlCheck(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

Statement prepare(sqlite3* db, char const* sql)
{
    sqlite3_stmt* statement{};
    sqlCheck(db, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr));
    return Statement(statement, sqlite3_finalize);
}

JsonObject readObject(sqlite3_stmt* statement, wchar_t const* error)
{
    const auto blob = static_cast<char const*>(sqlite3_column_blob(statement, 0));
    const auto length = sqlite3_column_bytes(statement, 0);
    JsonObject object{nullptr};
    if (!blob || length <= 0 ||
        !JsonObject::TryParse(to_hstring(std::string_view(blob, static_cast<std::size_t>(length))), object))
        invalid(error);
    return object;
}

void bindJson(sqlite3* db, sqlite3_stmt* statement, int column, JsonObject const& object)
{
    const auto bytes = to_string(object.Stringify());
    sqlCheck(db, sqlite3_bind_blob64(statement, column, bytes.data(), bytes.size(), SQLITE_TRANSIENT));
}

bool hasText(JsonObject const& object, hstring const& key)
{
    const auto text = object.GetNamedString(key, L"");
    return std::ranges::any_of(text, [](wchar_t letter) { return !iswspace(letter); });
}

bool hasUnclearText(hstring const& text)
{
    static const std::wregex unclear(
        LR"([\[［]\s*(unclear|unreadable|illegible|不清楚|不清晰|无法辨认|模糊)\s*[\]］])",
        std::regex_constants::icase);
    return std::regex_search(text.begin(), text.end(), unclear);
}

void putCount(JsonObject const& object, hstring const& key, int count)
{
    object.Insert(key, JsonValue::CreateNumberValue(count));
}

void putFlag(JsonObject const& object, hstring const& key, bool value)
{
    object.Insert(key, JsonValue::CreateBooleanValue(value));
}

}

PdfTaskStore::PdfTaskStore(std::filesystem::path const& path, bool readOnly)
{
    if (!readOnly && path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    sqlite3* raw{};
    const int flags = SQLITE_OPEN_FULLMUTEX | (readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    const int opened = sqlite3_open_v2(to_string(path.native()).c_str(), &raw, flags, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlCheck(raw, opened);
    sqlCheck(raw, sqlite3_busy_timeout(raw, 5000));
    if (!readOnly) {
        execute(raw, "PRAGMA journal_mode=WAL");
        execute(raw, "PRAGMA synchronous=FULL");
        execute(raw, "PRAGMA wal_autocheckpoint=256");
        execute(raw, "CREATE TABLE IF NOT EXISTS metadata (id INTEGER PRIMARY KEY CHECK(id=1), data BLOB NOT NULL)");
        execute(raw, "CREATE TABLE IF NOT EXISTS pages (number INTEGER PRIMARY KEY CHECK(number>=0),"
                     "data BLOB NOT NULL, summary BLOB NOT NULL)");
    }
    prepare(raw, "SELECT data FROM metadata WHERE id=1");
    prepare(raw, "SELECT data,summary FROM pages WHERE number=0");
    m_db = db.release();
}

PdfTaskStore::~PdfTaskStore()
{
    sqlite3_close(m_db);
}

JsonObject PdfTaskStore::metadata() const
{
    const auto query = prepare(m_db, "SELECT data FROM metadata WHERE id=1");
    const auto status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) return nullptr;
    sqlCheck(m_db, status, SQLITE_ROW);
    return readObject(query.get(), L"PDF 任务信息损坏。");
}

JsonObject PdfTaskStore::page(int number) const
{
    const auto query = prepare(m_db, "SELECT data FROM pages WHERE number=?1");
    sqlCheck(m_db, sqlite3_bind_int(query.get(), 1, number));
    const auto status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) return nullptr;
    sqlCheck(m_db, status, SQLITE_ROW);
    return readObject(query.get(), L"PDF 页面任务数据损坏。");
}

JsonArray PdfTaskStore::summaries() const
{
    JsonArray pages;
    const auto query = prepare(m_db, "SELECT summary FROM pages ORDER BY number");
    for (int status; (status = sqlite3_step(query.get())) != SQLITE_DONE;) {
        sqlCheck(m_db, status, SQLITE_ROW);
        pages.Append(readObject(query.get(), L"PDF 页面任务目录损坏。"));
    }
    return pages;
}

void PdfTaskStore::save(JsonObject const& metadata, std::span<PageUpdate const> pages)
{
    if (!metadata || metadata.Size() == 0) invalid(L"PDF 任务信息为空。");
    execute(m_db, "BEGIN IMMEDIATE");
    try {
        const auto update = prepare(m_db, "INSERT INTO pages(number,data,summary) VALUES(?1,?2,?3)"
            " ON CONFLICT(number) DO UPDATE SET data=excluded.data,summary=excluded.summary");
        for (const auto& page : pages) {
            if (page.number < 0 || !page.data || page.data.Size() == 0 ||
                page.data.GetNamedNumber(L"number", page.number) != page.number)
                invalid(L"PDF 页码或页面任务数据无效。");
            sqlCheck(m_db, sqlite3_bind_int(update.get(), 1, page.number));
            bindJson(m_db, update.get(), 2, page.data);
            bindJson(m_db, update.get(), 3, summary(page.number, page.data));
            sqlCheck(m_db, sqlite3_step(update.get()), SQLITE_DONE);
            sqlCheck(m_db, sqlite3_reset(update.get()));
        }
        const auto writeMetadata = prepare(m_db, "INSERT INTO metadata(id,data) VALUES(1,?1)"
            " ON CONFLICT(id) DO UPDATE SET data=excluded.data");
        bindJson(m_db, writeMetadata.get(), 1, metadata);
        sqlCheck(m_db, sqlite3_step(writeMetadata.get()), SQLITE_DONE);
        execute(m_db, "COMMIT");
    } catch (...) {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

JsonArray PdfTaskStore::blockIssues(JsonObject const& block)
{
    JsonArray issues;
    if (block.GetNamedBoolean(L"keep", false)) return issues;
    const auto add = [&](hstring const& kind, hstring const& reason) {
        JsonObject issue;
        issue.Insert(L"kind", JsonValue::CreateStringValue(kind));
        issue.Insert(L"reason", JsonValue::CreateStringValue(reason));
        issue.Insert(L"blockId", JsonValue::CreateStringValue(block.GetNamedString(L"id", L"")));
        issue.Insert(L"severity", JsonValue::CreateStringValue(L"review"));
        issues.Append(issue);
    };
    if (hasUnclearText(block.GetNamedString(L"source", L"")) || hasUnclearText(block.GetNamedString(L"translation", L"")))
        add(L"unclear", L"含不清晰文字，请对照原图校对");
    if (block.GetNamedBoolean(L"stale", false)) add(L"stale", L"原文已变动，译文需更新");
    const auto translationError = block.GetNamedString(L"translationError", L"");
    const auto layoutError = block.GetNamedString(L"layoutError", L"");
    if (!translationError.empty()) add(L"translation", translationError);
    if (!layoutError.empty()) add(L"layout", layoutError);
    if (block.GetNamedBoolean(L"backgroundReview", false) && !block.GetNamedBoolean(L"backgroundConfirmed", false))
        add(L"background", L"背景修复效果待确认");
    return issues;
}

JsonObject PdfTaskStore::summary(int number, JsonObject const& page)
{
    JsonObject result;
    putCount(result, L"number", number);
    for (const auto key : {L"width", L"height", L"warning", L"notice", L"kind", L"baseLayoutHash",
                          L"analyzed", L"analysisError", L"extractionVersion"}) {
        if (page.HasKey(key)) result.Insert(key, page.Lookup(key));
    }
    const bool scan = page.GetNamedString(L"kind", L"") == L"scan" || page.HasKey(L"scan");
    const auto data = scan ? page.GetNamedObject(L"scan", JsonObject{}) : page;
    const auto blocks = data.GetNamedArray(L"blocks", JsonArray{});
    JsonObject reasons;
    int total = 0, completed = 0, preserved = 0, review = 0;
    for (const auto& value : blocks) {
        const auto block = value.GetObject();
        const auto issues = blockIssues(block);
        if (issues.Size() != 0) ++review;
        for (const auto& issue : issues) {
            const auto kind = issue.GetObject().GetNamedString(L"kind");
            reasons.Insert(kind, JsonValue::CreateNumberValue(reasons.GetNamedNumber(kind, 0) + 1));
        }
        const bool keep = block.GetNamedBoolean(L"keep", false);
        if (keep || block.GetNamedString(L"backgroundMode", L"") == L"original") ++preserved;
        if (block.GetNamedBoolean(L"fixed", false) || keep) continue;
        ++total;
        if (!block.GetNamedBoolean(L"stale", false) && hasText(block, L"translation")) ++completed;
    }
    putFlag(result, L"scan", scan);
    putFlag(result, L"recognized", scan && hasText(data, L"source"));
    putFlag(result, L"hasBlocks", blocks.Size() != 0);
    putCount(result, L"total", total);
    putCount(result, L"completed", completed);
    putCount(result, L"preserved", preserved);
    putCount(result, L"review", review);
    putCount(result, L"issueVersion", 1);
    result.Insert(L"issues", reasons);
    return result;
}

}
