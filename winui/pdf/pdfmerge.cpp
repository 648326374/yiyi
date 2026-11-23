#include "pdfmerge.h"

#include <windows.h>
#include <qpdf/qpdf-c.h>

#include <bit>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace yiyi::pdf::merge {
namespace {

#define QPDF_FUNCTIONS(X) \
    X(qpdf_init) X(qpdf_cleanup) X(qpdf_read) X(qpdf_has_error) X(qpdf_get_error) X(qpdf_get_error_full_text) \
    X(qpdf_set_suppress_warnings) X(qpdf_silence_errors) X(qpdf_get_num_pages) X(qpdf_get_page_n) \
    X(qpdf_push_inherited_attributes_to_page) X(qpdf_oh_get_key) X(qpdf_oh_is_null) X(qpdf_oh_is_indirect) \
    X(qpdf_make_indirect_object) X(qpdf_oh_copy_foreign_object) X(qpdf_oh_replace_key) X(qpdf_oh_remove_key) \
    X(qpdf_oh_release_all) X(qpdf_init_write) X(qpdf_set_stream_data_mode) X(qpdf_set_object_stream_mode) \
    X(qpdf_empty_pdf) X(qpdf_add_page) X(qpdf_oh_is_array) X(qpdf_oh_get_page_content_data) \
    X(qpdf_oh_free_buffer) X(qpdf_oh_new_stream) X(qpdf_oh_new_null) X(qpdf_oh_replace_stream_data) \
    X(qpdf_oh_is_dictionary) X(qpdf_oh_is_stream) X(qpdf_oh_get_dict) X(qpdf_oh_get_object_id) \
    X(qpdf_oh_begin_dict_key_iter) X(qpdf_oh_dict_more_keys) X(qpdf_oh_dict_next_key) \
    X(qpdf_register_progress_reporter) X(qpdf_write)

constexpr char missingQpdf[] = "缺少 PDF 合并组件，请使用完整发行包或运行 scripts/setup-pdf-merge.ps1。";

struct Api {
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> library;

#define DECLARE(name) decltype(&::name) name = nullptr;
    QPDF_FUNCTIONS(DECLARE)
#undef DECLARE

    explicit Api(const std::filesystem::path& runtimeDirectory)
        : library(LoadLibraryExW(std::filesystem::absolute(runtimeDirectory / L"qpdf" / L"qpdf.dll").c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary)
    {
        if (!library) throw std::runtime_error(missingQpdf);
#define RESOLVE(name) name = std::bit_cast<decltype(name)>(GetProcAddress(library.get(), #name)); \
        if (!name) throw std::runtime_error(missingQpdf);
        QPDF_FUNCTIONS(RESOLVE)
#undef RESOLVE
    }

    void check(qpdf_data document) const
    {
        if (!qpdf_has_error(document)) return;
        const char* error = qpdf_get_error_full_text(document, qpdf_get_error(document));
        throw std::runtime_error(std::string("PDF 合并失败：") + (error ? error : "qpdf 未提供错误详情。"));
    }
};

#undef QPDF_FUNCTIONS

struct Document {
    Api& api;
    qpdf_data handle;

    explicit Document(Api& api, const std::filesystem::path& path = {}) : api(api), handle(api.qpdf_init())
    {
        if (!handle) throw std::runtime_error("无法初始化 PDF 合并组件。");
        try {
            api.qpdf_set_suppress_warnings(handle, QPDF_TRUE);
            api.qpdf_silence_errors(handle);
            if (path.empty()) api.qpdf_empty_pdf(handle);
            else {
                const auto name = path.u8string();
                api.qpdf_read(handle, reinterpret_cast<const char*>(name.c_str()), nullptr);
            }
            api.check(handle);
        } catch (...) {
            api.qpdf_cleanup(&handle);
            throw;
        }
    }

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document() { if (handle) api.qpdf_cleanup(&handle); }
};

bool usesPatterns(Api& api, qpdf_data document, qpdf_oh resources, std::set<int>& seen, int depth = 0)
{
    if (depth > 24) return true;
    const bool dictionary = api.qpdf_oh_is_dictionary(document, resources);
    api.check(document);
    if (!dictionary) return false;

    const auto patterns = api.qpdf_oh_get_key(document, resources, "/Pattern");
    api.check(document);
    const bool patternDictionary = api.qpdf_oh_is_dictionary(document, patterns);
    api.check(document);
    if (patternDictionary) {
        api.qpdf_oh_begin_dict_key_iter(document, patterns);
        api.check(document);
        const bool any = api.qpdf_oh_dict_more_keys(document);
        api.check(document);
        if (any) return true;
    }

    const auto objects = api.qpdf_oh_get_key(document, resources, "/XObject");
    api.check(document);
    const bool objectDictionary = api.qpdf_oh_is_dictionary(document, objects);
    api.check(document);
    if (!objectDictionary) return false;

    std::vector<std::string> keys;
    api.qpdf_oh_begin_dict_key_iter(document, objects);
    api.check(document);
    while (api.qpdf_oh_dict_more_keys(document)) {
        const auto* key = api.qpdf_oh_dict_next_key(document);
        api.check(document);
        if (key) keys.emplace_back(key);
    }
    api.check(document);
    for (const auto& key : keys) {
        const auto object = api.qpdf_oh_get_key(document, objects, key.c_str());
        api.check(document);
        const int id = api.qpdf_oh_get_object_id(document, object);
        api.check(document);
        if (id && !seen.insert(id).second) continue;
        const bool stream = api.qpdf_oh_is_stream(document, object);
        api.check(document);
        if (!stream) continue;
        const auto objectAttributes = api.qpdf_oh_get_dict(document, object);
        api.check(document);
        const auto nested = api.qpdf_oh_get_key(document, objectAttributes, "/Resources");
        api.check(document);
        if (usesPatterns(api, document, nested, seen, depth + 1)) return true;
    }
    return false;
}

void initializeWrite(Api& api, qpdf_data output, const std::filesystem::path& destination)
{
    const auto path = destination.u8string();
    api.qpdf_init_write(output, reinterpret_cast<const char*>(path.c_str()));
    api.check(output);
    api.qpdf_set_stream_data_mode(output, qpdf_s_preserve);
    api.check(output);
    api.qpdf_set_object_stream_mode(output, qpdf_o_generate);
    api.check(output);
}

void coalesceContents(Api& api, qpdf_data input, qpdf_oh page)
{
    const auto contents = api.qpdf_oh_get_key(input, page, "/Contents");
    api.check(input);
    const bool split = api.qpdf_oh_is_array(input, contents);
    api.check(input);
    if (!split) return;

    struct Buffer {
        Api& api;
        unsigned char* data = nullptr;
        ~Buffer() { if (data) api.qpdf_oh_free_buffer(&data); }
    } buffer{api};
    std::size_t size = 0;
    api.qpdf_oh_get_page_content_data(input, page, &buffer.data, &size);
    api.check(input);
    const auto stream = api.qpdf_oh_new_stream(input);
    api.check(input);
    const auto null = api.qpdf_oh_new_null(input);
    api.check(input);
    api.qpdf_oh_replace_stream_data(input, stream, buffer.data, size, null, null);
    api.check(input);
    api.qpdf_oh_replace_key(input, page, "/Contents", stream);
    api.check(input);
}

}

std::set<int> patternPages(const std::filesystem::path& runtimeDirectory, const std::filesystem::path& source,
                           int first, int last)
{
    Api api(runtimeDirectory);
    Document input(api, source);
    const auto document = input.handle;
    const int count = api.qpdf_get_num_pages(document);
    api.check(document);
    if (first < 0 || last < first || last >= count) throw std::runtime_error("PDF 图形检查页码无效。");
    api.qpdf_push_inherited_attributes_to_page(document);
    api.check(document);
    std::set<int> result;
    for (int index = first; index <= last; ++index) {
        const auto page = api.qpdf_get_page_n(document, static_cast<std::size_t>(index));
        api.check(document);
        const auto resources = api.qpdf_oh_get_key(document, page, "/Resources");
        api.check(document);
        std::set<int> seen;
        if (usesPatterns(api, document, resources, seen)) result.insert(index);
        api.qpdf_oh_release_all(document);
    }
    return result;
}

void copyPages(const std::filesystem::path& runtimeDirectory, const std::filesystem::path& source,
               const std::filesystem::path& destination, int first, int last)
{
    Api api(runtimeDirectory);
    Document input(api, source), output(api);
    const int count = api.qpdf_get_num_pages(input.handle);
    api.check(input.handle);
    if (first < 0 || last < first || last >= count) throw std::runtime_error("PDF 页面工作副本的页码无效。");
    api.qpdf_push_inherited_attributes_to_page(input.handle);
    api.check(input.handle);
    for (int index = first; index <= last; ++index) {
        const auto page = api.qpdf_get_page_n(input.handle, static_cast<std::size_t>(index));
        api.check(input.handle);
        coalesceContents(api, input.handle, page);
        api.qpdf_add_page(output.handle, input.handle, page, QPDF_FALSE);
        api.check(output.handle);
        api.qpdf_oh_release_all(input.handle);
    }
    initializeWrite(api, output.handle, destination);
    api.qpdf_write(output.handle);
    api.check(output.handle);
}

void write(const std::filesystem::path& runtimeDirectory, const std::filesystem::path& source,
           const std::vector<std::filesystem::path>& parts, const std::filesystem::path& destination,
           int pageCount, const Progress& progress, std::span<const std::uint8_t> modified)
{
    Api api(runtimeDirectory);
    // Foreign stream data stays backed by the input files through the final qpdf write.
    std::vector<std::unique_ptr<Document>> inputs;
    Document output(api, source);
    const int count = api.qpdf_get_num_pages(output.handle);
    api.check(output.handle);
    if (count != pageCount || (!modified.empty() && modified.size() != static_cast<std::size_t>(count)))
        throw std::runtime_error("PDF 合并页数与原文件不一致。");

    int offset = 0;
    for (const auto& path : parts) {
        auto input = std::make_unique<Document>(api, path);
        const auto from = input->handle;
        api.qpdf_push_inherited_attributes_to_page(from);
        api.check(from);
        const int pages = api.qpdf_get_num_pages(from);
        api.check(from);
        if (pages < 1 || pages > count - offset) throw std::runtime_error("PDF 分片页数无效。");
        for (int index = 0; index < pages; ++index) {
            if (!modified.empty() && !modified[offset + index]) continue;
            const auto original = api.qpdf_get_page_n(output.handle, static_cast<std::size_t>(offset + index));
            api.check(output.handle);
            const auto translated = api.qpdf_get_page_n(from, static_cast<std::size_t>(index));
            api.check(from);
            for (const char* key : {"/Contents", "/Resources"}) {
                auto value = api.qpdf_oh_get_key(from, translated, key);
                api.check(from);
                const bool empty = api.qpdf_oh_is_null(from, value);
                api.check(from);
                if (empty) {
                    api.qpdf_oh_remove_key(output.handle, original, key);
                    api.check(output.handle);
                    continue;
                }
                const bool indirect = api.qpdf_oh_is_indirect(from, value);
                api.check(from);
                if (!indirect) {
                    value = api.qpdf_make_indirect_object(from, value);
                    api.check(from);
                }
                const auto copied = api.qpdf_oh_copy_foreign_object(output.handle, from, value);
                api.check(output.handle);
                api.qpdf_oh_replace_key(output.handle, original, key, copied);
                api.check(output.handle);
            }
            if (progress) progress("merge", offset + index + 1, count);
        }
        offset += pages;
        api.qpdf_oh_release_all(from);
        api.qpdf_oh_release_all(output.handle);
        inputs.push_back(std::move(input));
    }

    if (offset != count) throw std::runtime_error("PDF 分片不完整，导出已停止。");
    initializeWrite(api, output.handle, destination);
    struct Callback {
        const Progress& progress;
        std::exception_ptr error;
    } callback{progress, {}};
    if (progress) {
        api.qpdf_register_progress_reporter(output.handle, [](int percent, void* data) noexcept {
            auto& callback = *static_cast<Callback*>(data);
            if (callback.error) return;
            try {
                callback.progress("assembling", percent, 100);
            } catch (...) {
                // An MSVC exception cannot cross the MinGW DLL boundary.
                callback.error = std::current_exception();
            }
        }, &callback);
        api.check(output.handle);
    }
    api.qpdf_write(output.handle);
    api.check(output.handle);
    if (callback.error) std::rethrow_exception(callback.error);
}

}
