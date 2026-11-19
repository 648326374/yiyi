#include "pdfmerge.h"
#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <memory>
#include <vector>
#include <qpdf/qpdf-c.h>

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

struct Api {
    QLibrary library;
#define DECLARE(name) decltype(&name) name = nullptr;
    QPDF_FUNCTIONS(DECLARE)
#undef DECLARE
    Api() : library(QDir(QCoreApplication::applicationDirPath()).filePath("pdf/qpdf/qpdf.dll")) {
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) throw QStringLiteral("缺少 PDF 合并组件，请使用完整发行包或运行 scripts/setup-pdf-merge.ps1。");
        QPDF_FUNCTIONS(RESOLVE)
#undef RESOLVE
    }
    void check(qpdf_data data) const {
        if (!qpdf_has_error(data)) return;
        const auto error = qpdf_get_error(data);
        throw QStringLiteral("PDF 合并失败：%1").arg(QString::fromUtf8(qpdf_get_error_full_text(data, error)));
    }
};
struct Document {
    Api &api;
    qpdf_data handle;
    Document(Api &api, const QString &path = {}) : api(api), handle(api.qpdf_init()) {
        if (!handle) throw QStringLiteral("无法初始化 PDF 合并组件。");
        api.qpdf_set_suppress_warnings(handle, QPDF_TRUE);
        api.qpdf_silence_errors(handle);
        if (path.isEmpty()) api.qpdf_empty_pdf(handle);
        else api.qpdf_read(handle, path.toUtf8().constData(), nullptr);
        try { api.check(handle); } catch (...) { api.qpdf_cleanup(&handle); throw; }
    }
    ~Document() { if (handle) api.qpdf_cleanup(&handle); }
};

bool usesPatterns(Api &api, qpdf_data doc, qpdf_oh resources, QSet<int> &seen, int depth = 0) {
    if (depth > 24) return true;
    const bool dictionary = api.qpdf_oh_is_dictionary(doc, resources); api.check(doc);
    if (!dictionary) return false;
    const auto patterns = api.qpdf_oh_get_key(doc, resources, "/Pattern"); api.check(doc);
    const bool patternDictionary = api.qpdf_oh_is_dictionary(doc, patterns); api.check(doc);
    if (patternDictionary) {
        api.qpdf_oh_begin_dict_key_iter(doc, patterns); api.check(doc);
        const bool any = api.qpdf_oh_dict_more_keys(doc); api.check(doc);
        if (any) return true;
    }
    const auto objects = api.qpdf_oh_get_key(doc, resources, "/XObject"); api.check(doc);
    const bool objectDictionary = api.qpdf_oh_is_dictionary(doc, objects); api.check(doc);
    if (!objectDictionary) return false;
    QList<QByteArray> keys;
    api.qpdf_oh_begin_dict_key_iter(doc, objects); api.check(doc);
    while (api.qpdf_oh_dict_more_keys(doc)) { keys.append(api.qpdf_oh_dict_next_key(doc)); api.check(doc); }
    api.check(doc);
    for (const auto &key : keys) {
        const auto object = api.qpdf_oh_get_key(doc, objects, key.constData()); api.check(doc);
        const int id = api.qpdf_oh_get_object_id(doc, object); api.check(doc);
        if (id && seen.contains(id)) continue;
        if (id) seen.insert(id);
        const bool stream = api.qpdf_oh_is_stream(doc, object); api.check(doc);
        if (!stream) continue;
        const auto dict = api.qpdf_oh_get_dict(doc, object); api.check(doc);
        const auto nested = api.qpdf_oh_get_key(doc, dict, "/Resources"); api.check(doc);
        if (usesPatterns(api, doc, nested, seen, depth + 1)) return true;
    }
    return false;
}
}

QSet<int> PdfMerge::patternPages(const QString &source, int first, int last) {
    Api api; Document doc(api, source); QSet<int> result;
    const int count = api.qpdf_get_num_pages(doc.handle); api.check(doc.handle);
    if (first < 0 || last < first || last >= count) throw QStringLiteral("PDF 图形检查页码无效。");
    api.qpdf_push_inherited_attributes_to_page(doc.handle); api.check(doc.handle);
    for (int i = first; i <= last; ++i) {
        const auto page = api.qpdf_get_page_n(doc.handle, size_t(i)); api.check(doc.handle);
        const auto resources = api.qpdf_oh_get_key(doc.handle, page, "/Resources"); api.check(doc.handle);
        QSet<int> seen;
        if (usesPatterns(api, doc.handle, resources, seen)) result.insert(i);
        api.qpdf_oh_release_all(doc.handle);
    }
    return result;
}

void PdfMerge::copyPages(const QString &source, const QString &destination, int first, int last) {
    Api api; Document input(api, source), output(api);
    const int count = api.qpdf_get_num_pages(input.handle); api.check(input.handle);
    if (first < 0 || last < first || last >= count) throw QStringLiteral("PDF 页面工作副本的页码无效。");
    api.qpdf_push_inherited_attributes_to_page(input.handle); api.check(input.handle);
    for (int i = first; i <= last; ++i) {
        const auto page = api.qpdf_get_page_n(input.handle, size_t(i)); api.check(input.handle);
        const auto contents = api.qpdf_oh_get_key(input.handle, page, "/Contents"); api.check(input.handle);
        const bool split = api.qpdf_oh_is_array(input.handle, contents); api.check(input.handle);
        if (split) {
            struct Buffer {
                Api &api; unsigned char *data = nullptr;
                ~Buffer() { if (data) api.qpdf_oh_free_buffer(&data); }
            } buffer{api};
            size_t size = 0;
            api.qpdf_oh_get_page_content_data(input.handle, page, &buffer.data, &size); api.check(input.handle);
            const auto stream = api.qpdf_oh_new_stream(input.handle); api.check(input.handle);
            const auto null = api.qpdf_oh_new_null(input.handle); api.check(input.handle);
            api.qpdf_oh_replace_stream_data(input.handle, stream, buffer.data, size, null, null); api.check(input.handle);
            api.qpdf_oh_replace_key(input.handle, page, "/Contents", stream); api.check(input.handle);
        }
        api.qpdf_add_page(output.handle, input.handle, page, QPDF_FALSE); api.check(output.handle);
        api.qpdf_oh_release_all(input.handle);
    }
    api.qpdf_init_write(output.handle, destination.toUtf8().constData()); api.check(output.handle);
    api.qpdf_set_stream_data_mode(output.handle, qpdf_s_preserve); api.check(output.handle);
    api.qpdf_set_object_stream_mode(output.handle, qpdf_o_generate); api.check(output.handle);
    api.qpdf_write(output.handle); api.check(output.handle);
}

void PdfMerge::write(const QString &source, const QStringList &parts, const QString &destination,
                     int pageCount, const PdfEngine::Progress &progress) {
    Api api;
    // Keep source documents alive until writing completes: foreign streams are
    // read lazily from these files, rather than copied into a whole-book buffer.
    std::vector<std::unique_ptr<Document>> inputs;
    Document output(api, source);
    const int count = api.qpdf_get_num_pages(output.handle); api.check(output.handle);
    if (count != pageCount) throw QStringLiteral("PDF 合并页数与原文件不一致。");
    int offset = 0;
    for (const auto &path : parts) {
        auto input = std::make_unique<Document>(api, path);
        const auto from = input->handle;
        api.qpdf_push_inherited_attributes_to_page(from); api.check(from);
        const int pages = api.qpdf_get_num_pages(from); api.check(from);
        if (pages < 1 || offset + pages > count) throw QStringLiteral("PDF 分片页数无效。");
        for (int p = 0; p < pages; ++p) {
            const auto original = api.qpdf_get_page_n(output.handle, size_t(offset + p)); api.check(output.handle);
            const auto translated = api.qpdf_get_page_n(from, size_t(p)); api.check(from);
            for (const auto *key : {"/Contents", "/Resources"}) {
                auto value = api.qpdf_oh_get_key(from, translated, key); api.check(from);
                const bool empty = api.qpdf_oh_is_null(from, value); api.check(from);
                if (empty) { api.qpdf_oh_remove_key(output.handle, original, key); api.check(output.handle); continue; }
                const bool indirect = api.qpdf_oh_is_indirect(from, value); api.check(from);
                if (!indirect) { value = api.qpdf_make_indirect_object(from, value); api.check(from); }
                const auto copied = api.qpdf_oh_copy_foreign_object(output.handle, from, value); api.check(output.handle);
                api.qpdf_oh_replace_key(output.handle, original, key, copied); api.check(output.handle);
            }
            if (progress) progress({{"stage", "merge"}, {"completed", offset + p + 1}, {"total", count}});
        }
        offset += pages;
        api.qpdf_oh_release_all(from); api.qpdf_oh_release_all(output.handle);
        inputs.push_back(std::move(input));
    }
    if (offset != count) throw QStringLiteral("PDF 分片不完整，导出已停止。");
    api.qpdf_init_write(output.handle, destination.toUtf8().constData()); api.check(output.handle);
    api.qpdf_set_stream_data_mode(output.handle, qpdf_s_preserve); api.check(output.handle);
    api.qpdf_set_object_stream_mode(output.handle, qpdf_o_generate); api.check(output.handle);
    struct Callback { const PdfEngine::Progress &progress; } callback{progress};
    api.qpdf_register_progress_reporter(output.handle, [](int percent, void *data) {
        const auto &notify = static_cast<Callback *>(data)->progress;
        if (notify) notify({{"stage", "assembling"}, {"completed", percent}, {"total", 100}});
    }, &callback); api.check(output.handle);
    api.qpdf_write(output.handle); api.check(output.handle);
}
