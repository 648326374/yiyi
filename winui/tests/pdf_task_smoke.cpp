#include "../pdf/pdftaskstore.h"
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <cstdio>

namespace {

using namespace winrt;
using namespace Windows::Data::Json;
using namespace yiyi::pdf;

void require(bool ok, wchar_t const* message)
{
    if (!ok) throw hresult_error(E_FAIL, message);
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    try {
        init_apartment();
        PdfTaskStore writer(argv[1]);
        const PdfTaskStore reader(argv[1], true);
        const auto metadata = writer.metadata();
        require(metadata.GetNamedNumber(L"storageVersion") == 6, L"Qt task metadata did not load.");
        require(reader.page(1).GetNamedArray(L"blocks").GetObjectAt(0).GetNamedString(L"translation") == L"保留",
                L"Qt task page did not load.");
        require(!reader.page(0), L"Unwritten page should be absent.");

        const auto changed = JsonObject::Parse(metadata.Stringify());
        changed.Insert(L"workflow", JsonObject::Parse(LR"({"kind":"translate","state":"paused","cursor":0})"));
        const auto page = JsonObject::Parse(LR"({
            "number":0,"width":200,"height":150,"kind":"scan","scan":{"source":"原始文本","blocks":[
            {"id":"p0-b0","source":"[ UNCLEAR ]","translation":"旧译文","stale":true,"backgroundReview":true},
            {"id":"p0-b1","source":"next","translation":"下一个"},
            {"id":"p0-b2","source":"keep","translation":"保留","keep":true}]}})");
        const std::array invalidBatch{PageUpdate{0, page}, PageUpdate{-1, page}};
        bool failed = false;
        try { writer.save(changed, invalidBatch); }
        catch (hresult_error const&) { failed = true; }
        require(failed && !reader.page(0) && !reader.metadata().HasKey(L"workflow"), L"Failed page transaction did not roll back.");

        const std::array validBatch{PageUpdate{0, page}};
        writer.save(changed, validBatch);
        require(reader.metadata().GetNamedObject(L"workflow").GetNamedString(L"state") == L"paused",
                L"Reader did not see committed workflow state.");
        const auto summaries = reader.summaries();
        const auto first = summaries.GetObjectAt(0);
        require(summaries.Size() == 2 && first.GetNamedNumber(L"number") == 0 &&
            first.GetNamedBoolean(L"scan") && first.GetNamedBoolean(L"recognized") &&
            first.GetNamedNumber(L"review") == 1 && first.GetNamedNumber(L"total") == 2 &&
            first.GetNamedNumber(L"completed") == 1 && first.GetNamedNumber(L"preserved") == 1,
            L"Native task summary/issue counts differ from legacy.");
        const auto issues = first.GetNamedObject(L"issues");
        for (const auto key : {L"unclear", L"stale", L"background"})
            require(issues.GetNamedNumber(key) == 1, L"Missing page issue.");
        return 0;
    } catch (...) {
        std::fwprintf(stderr, L"%ls\n", to_message().c_str());
        return 1;
    }
}
