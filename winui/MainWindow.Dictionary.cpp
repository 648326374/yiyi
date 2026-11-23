#include "pch.h"
#include "MainWindow.xaml.h"
#include "Dictionary.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Text.h>

namespace winrt::Yiyi::WinUI::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Windows::Data::Json;

    namespace
    {
        hstring textField(JsonObject const& object, hstring const& key)
        {
            if (!object.HasKey(key)) return {};
            const auto value = object.Lookup(key);
            return value.ValueType() == JsonValueType::String ? value.GetString() : hstring{};
        }

        JsonArray listField(JsonObject const& object, hstring const& key)
        {
            if (!object.HasKey(key)) return nullptr;
            const auto value = object.Lookup(key);
            return value.ValueType() == JsonValueType::Array ? value.GetArray() : JsonArray{nullptr};
        }

        Style dictionaryStyle(hstring const& key)
        {
            return Application::Current().Resources().Lookup(box_value(key)).as<Style>();
        }

        TextBlock entryText(hstring const& value, double size, int weight = 400, bool muted = false)
        {
            TextBlock text;
            text.Style(dictionaryStyle(muted ? L"YiyiDictionaryMuted" : L"YiyiDictionaryText"));
            text.FontFamily(Media::FontFamily(L"Microsoft YaHei UI"));
            text.FontSize(size);
            text.FontWeight({static_cast<uint16_t>(weight)});
            text.TextWrapping(TextWrapping::Wrap);
            text.IsTextSelectionEnabled(true);
            text.Text(value);
            return text;
        }

        void appendPairSection(StackPanel const& panel, JsonArray const& values, hstring const& title,
                               hstring const& mainField, hstring const& detailField)
        {
            if (!values || values.Size() == 0) return;
            const bool examples = mainField == L"source";
            StackPanel section;
            section.Spacing(examples ? 16 : 12);
            section.Children().Append(entryText(title, 12, 700, true));
            for (auto const& value : values) {
                if (value.ValueType() != JsonValueType::Object) continue;
                const auto entry = value.GetObject();
                const auto text = textField(entry, mainField);
                if (text.empty()) continue;
                StackPanel pair;
                pair.Spacing(5);
                auto main = entryText(text, 15);
                main.FontFamily(Media::FontFamily(L"Segoe UI"));
                if (examples) {
                    main.LineHeight(21);
                    main.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
                }
                pair.Children().Append(main);
                const auto detail = textField(entry, detailField);
                if (!detail.empty()) {
                    auto description = entryText(detail, 12, 400, true);
                    description.LineHeight(19);
                    description.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
                    pair.Children().Append(description);
                }
                if (examples) {
                    Border example;
                    example.Style(dictionaryStyle(L"YiyiDictionaryExample"));
                    example.Child(pair);
                    section.Children().Append(example);
                } else section.Children().Append(pair);
            }
            panel.Children().Append(section);
        }
    }

    void MainWindow::showDictionaryEntry(JsonObject const& entry)
    {
        const auto definitions = entry ? listField(entry, L"definitions") : JsonArray{nullptr};
        const bool hasEntry = definitions && definitions.Size() > 0;
        const auto signature = hasEntry ? entry.Stringify() : hstring{};
        ResultText().Visibility(hasEntry ? Visibility::Collapsed : Visibility::Visible);
        DictionaryScroll().Visibility(hasEntry ? Visibility::Visible : Visibility::Collapsed);
        const bool brief = hasEntry && ::yiyi::isBriefDictionary(entry);
        WorkbenchResultKind().Text(!hasEntry ? L"译文" : brief ? L"释义" : L"词条");
        updateWorkbenchChrome();
        if (m_dictionaryViewSignature == signature) return;
        m_dictionaryViewSignature = signature;
        const auto panel = DictionaryContent();
        panel.Children().Clear();
        if (!hasEntry) return;
        if (!brief) {
            StackPanel heading;
            heading.Spacing(9);
            auto word = entryText(textField(entry, L"word"), 32);
            word.FontFamily(Media::FontFamily(L"Georgia"));
            Automation::AutomationProperties::SetAutomationId(word, L"dictionaryWord");
            heading.Children().Append(word);
            const auto phonetic = textField(entry, L"phonetic");
            if (!phonetic.empty()) {
                auto pronunciation = entryText(phonetic, 15, 400, true);
                pronunciation.FontFamily(Media::FontFamily(L"Segoe UI"));
                heading.Children().Append(pronunciation);
            }
            panel.Children().Append(heading);
            const auto partOfSpeech = textField(entry, L"partOfSpeech");
            if (!partOfSpeech.empty()) {
                auto pos = entryText(partOfSpeech, 11);
                pos.Style(dictionaryStyle(L"YiyiDictionaryAccent"));
                Border badge;
                badge.Style(dictionaryStyle(L"YiyiDictionaryPos"));
                badge.Child(pos);
                panel.Children().Append(badge);
            }
        }

        uint32_t index = 0;
        for (auto const& value : definitions) {
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto definition = value.GetObject();
            Grid row;
            row.ColumnSpacing(13);
            ColumnDefinition prefix;
            prefix.Width({1, GridUnitType::Auto});
            row.ColumnDefinitions().Append(prefix);
            ColumnDefinition body;
            body.Width({1, GridUnitType::Star});
            row.ColumnDefinitions().Append(body);
            const auto partOfSpeech = brief ? textField(definition, L"partOfSpeech") :
                hstring((index + 1 < 10 ? L"0" : L"") + std::to_wstring(index + 1));
            if (!partOfSpeech.empty()) {
                auto label = entryText(partOfSpeech, 13);
                label.Style(dictionaryStyle(L"YiyiDictionaryAccent"));
                if (brief) label.FontStyle(Windows::UI::Text::FontStyle::Italic);
                row.Children().Append(label);
            }
            StackPanel summary;
            summary.Spacing(7);
            auto meaning = entryText(textField(definition, L"meaning"), 15, 700);
            meaning.LineHeight(22);
            meaning.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            Automation::AutomationProperties::SetAutomationId(meaning, hstring(L"dictionaryMeaning-" + std::to_wstring(index++)));
            summary.Children().Append(meaning);
            const auto detail = textField(definition, L"detail");
            if (!detail.empty()) {
                auto description = entryText(detail, 12, 400, true);
                description.LineHeight(19);
                description.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
                summary.Children().Append(description);
            }
            Grid::SetColumn(summary, 1);
            row.Children().Append(summary);
            panel.Children().Append(row);
        }
        const auto examples = listField(entry, L"examples");
        if (examples && examples.Size() != 0) {
            Border divider;
            divider.Style(dictionaryStyle(L"YiyiDictionaryDivider"));
            panel.Children().Append(divider);
        }
        appendPairSection(panel, examples, L"语境例句", L"source", L"translation");
        const auto note = textField(entry, L"note");
        if (!note.empty()) {
            Border highlight;
            highlight.Style(dictionaryStyle(L"YiyiDictionaryNote"));
            auto body = entryText(note, 12);
            body.Style(dictionaryStyle(L"YiyiDictionaryAccent"));
            highlight.Child(body);
            panel.Children().Append(highlight);
        }
        appendPairSection(panel, listField(entry, L"collocations"), L"常见搭配", L"phrase", L"meaning");
        if (const auto synonyms = listField(entry, L"synonyms")) {
            std::wstring related;
            for (auto const& value : synonyms) {
                if (value.ValueType() != JsonValueType::String) continue;
                if (!related.empty()) related += L"  ·  ";
                related += std::wstring_view(value.GetString());
            }
            if (!related.empty()) panel.Children().Append(entryText(hstring(related), 12, 400, true));
        }
    }
}
