"""Exercise the DOCX preview on an unlocked Windows desktop with local Word."""

from argparse import ArgumentParser
from pathlib import Path
from tempfile import TemporaryDirectory
from xml.sax.saxutils import escape
from zipfile import ZIP_DEFLATED, ZipFile
import winreg

from workbench_smoke import Desktop, ctypes, fake_provider, gui, screenshot, wait, wc, workbench


EXPANDING_PARAGRAPH = "项目负责人在评审时逐项核对申报材料。"
LONG_TRANSLATION = ("The project lead checks every proof of completion with the review team. " * 48).strip()
TABLE = (
    ("检查项", "核对内容"),
    ("申报记录", "确认项目与负责人的信息。"),
    ("证据来源", "提供可以查阅的原始材料。"),
    ("完成时间", "注明每项工作完成的日期。"),
    ("评审意见", "记录需要补充或修改的内容。"),
)
TRANSLATIONS = {
    "材料评审记录": "Review notes",
    EXPANDING_PARAGRAPH: LONG_TRANSLATION,
    "核对后请记录问题。": "Record any issues after the review.",
    "已确认的材料可用于提交。": "Confirmed evidence is ready for submission.",
    "附注": "Notes",
    "译稿保留表格结构，可继续编辑。": "The translated table remains editable.",
    "检查项": "Item",
    "核对内容": "Review criteria",
    "申报记录": "Application",
    "确认项目与负责人的信息。": "Verify the project and its lead.",
    "证据来源": "Sources",
    "提供可以查阅的原始材料。": "Provide the original materials for review.",
    "完成时间": "Completion",
    "注明每项工作完成的日期。": "Record the completion date for each item.",
    "评审意见": "Feedback",
    "记录需要补充或修改的内容。": "Note items to add or revise.",
}


def pagination_fixture(template, path):
    ns = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'

    def paragraph(text, style="", run_properties=""):
        properties = f"<w:pPr>{style}</w:pPr>" if style else ""
        run = f"<w:rPr>{run_properties}</w:rPr>" if run_properties else ""
        return f"<w:p>{properties}<w:r>{run}<w:t>{escape(text)}</w:t></w:r></w:p>"

    page_break = '<w:p><w:r><w:br w:type="page"/></w:r></w:p>'
    document = [
        paragraph("材料评审记录", '<w:pStyle w:val="Title"/>'),
        paragraph(EXPANDING_PARAGRAPH, '<w:spacing w:line="320" w:lineRule="exact"/>',
                  '<w:rFonts w:ascii="Consolas" w:hAnsi="Consolas"/><w:sz w:val="24"/>'),
        paragraph("核对后请记录问题。"),
        page_break,
        '<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="9360" w:type="dxa"/>'
        '<w:tblLayout w:type="fixed"/></w:tblPr><w:tblGrid><w:gridCol w:w="3000"/><w:gridCol w:w="6360"/></w:tblGrid>',
    ]
    for index, row in enumerate(TABLE):
        document.append("<w:tr>")
        for width, text in zip((3000, 6360), row):
            shading = '<w:shd w:fill="EAF1EB"/>' if index == 0 else ""
            caption = paragraph(text, '<w:spacing w:before="140" w:after="140"/>', "<w:b/>" if index == 0 else "")
            document.append(f'<w:tc><w:tcPr><w:tcW w:w="{width}" w:type="dxa"/>{shading}</w:tcPr>{caption}</w:tc>')
        document.append("</w:tr>")
    document += [
        "</w:tbl>",
        paragraph("已确认的材料可用于提交。"),
        page_break,
        paragraph("附注", '<w:pStyle w:val="Heading1"/>'),
        paragraph("译稿保留表格结构，可继续编辑。"),
        '<w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440"'
        ' w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>',
    ]
    contents = f'<?xml version="1.0" encoding="UTF-8"?><w:document {ns}><w:body>{"".join(document)}</w:body></w:document>'
    with ZipFile(template) as source, ZipFile(path, "w", ZIP_DEFLATED) as fixture:
        for name in source.namelist():
            fixture.writestr(name, contents.encode("utf-8") if name == "word/document.xml" else source.read(name))


def word_installed():
    key = r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\WINWORD.EXE"
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                with winreg.OpenKey(root, key, access=winreg.KEY_READ | view) as entry:
                    if Path(winreg.ExpandEnvironmentStrings(winreg.QueryValue(entry, ""))).is_file():
                        return True
            except OSError:
                continue
    return False


def verify(executable, document, screenshots, theme):
    original_bytes = document.read_bytes()
    with fake_provider(lambda source: TRANSLATIONS.get(source, source)) as (requests, url):
        settings = {"theme": theme, "selectedProviderId": "preview-test", "providers": [
            {"id": "preview-test", "name": "本机演示", "model": "preview", "protocol": "openai", "baseUrl": url}
        ]}
        with workbench(executable, settings, "--document", document) as (_, root, ui, keys):
            dpi = ctypes.windll.user32.GetDpiForWindow(root.handle)
            if theme == "dark":
                gui.SetWindowPos(root.handle, 0, 0, 0, round(900 * dpi / 96), round(600 * dpi / 96),
                                 wc.SWP_NOMOVE | wc.SWP_NOZORDER)

            def find(title, kind=None):
                return wait(title, lambda: root.descendants(title=title, control_type=kind))[0]

            def popup(title, kind):
                windows = Desktop(backend="uia")
                return wait(title, lambda: next(
                    (item for window in windows.windows(process=root.process_id())
                     for item in window.descendants(title=title, control_type=kind)), None))

            def click(title):
                find(title, "Button").iface_invoke.Invoke()

            def page_label(identifier):
                return ui(identifier).window_text()

            def ready(title):
                return any(control.rectangle().height() > 20 for control in
                           root.descendants(title=title, control_type="Image"))

            def reading(title):
                wait(title, lambda: ready(title), timeout=25)

            def pages(original, translated):
                wait(f"pages {original} and {translated}", lambda:
                     page_label("OfficeOriginalPage") == original and page_label("OfficeTranslatedPage") == translated)
                reading("原文整页预览")
                reading("译文整页预览")

            reading("原文整页预览")
            assert page_label("OfficeOriginalPage") == "1 / 3"
            assert not ready("译文整页预览") and not requests
            assert find("更换文档", "Button").is_enabled()
            assert not root.descendants(title="打开文档", control_type="Button")

            click("翻译设置")
            assert popup("目标语言", "ComboBox").is_enabled()
            assert popup("AI 服务", "ComboBox").selected_text() == "本机演示"
            keys("{ESC}")
            click("开始翻译")
            pages("1 / 3", "1 / 4")
            assert requests
            original = find("原文整页预览", "Image").rectangle()
            assert find("译文整页预览", "Image").rectangle().left > original.left
            assert find("另存译文", "Button").rectangle().bottom <= original.top
            assert not root.descendants(title="重新翻译", control_type="Button")
            click("更多文档操作")
            assert popup("图片取字", "MenuItem").is_enabled()
            popup("重新翻译", "MenuItem").iface_invoke.Invoke()
            find("重新翻译文档")
            click("取消")
            request_count = len(requests)

            mode = find("翻页方式", "ComboBox")
            zoom = find("预览缩放", "ComboBox")
            assert mode.selected_text() == "内容联动"
            assert zoom.selected_text() == "适合栏宽"
            assert min(mode.rectangle().width(), zoom.rectangle().width()) >= round(112 * dpi / 96)
            assert mode.rectangle().right < zoom.rectangle().left
            click("原文下一页")
            pages("2 / 3", "3 / 4")
            screenshot(root, screenshots, "docx-preview" if theme == "light" else "docx-preview-dark")

            width = find("原文整页预览", "Image").rectangle().width()
            zoom.select("适合整页")
            wait("fit the full page", lambda: find("原文整页预览", "Image").rectangle().width() < width)
            pages("2 / 3", "3 / 4")
            click("原文上一页")
            pages("1 / 3", "1 / 4")
            zoom.select("200%")
            viewer = find("原文页面")
            wait("scrollable original", lambda: viewer.iface_scroll.CurrentVerticallyScrollable)
            viewer.iface_scroll.SetScrollPercent(-1, 100)
            pages("1 / 3", "2 / 4")
            zoom.select("适合栏宽")

            click("原文下一页")
            pages("2 / 3", "3 / 4")
            mode.select("独立翻页")
            click("译文下一页")
            pages("2 / 3", "4 / 4")
            mode.select("内容联动")
            pages("3 / 3", "4 / 4")
            click("原文上一页")
            pages("2 / 3", "3 / 4")

            click("逐段校对")
            segments = find("文档段落", "List")
            segments.descendants(control_type="ListItem")[1].iface_selection_item.Select()
            editor = find("译文校对", "Edit")
            assert editor.iface_value.CurrentValue == LONG_TRANSLATION
            draft = (LONG_TRANSLATION + " ") * 2
            draft = draft.strip()
            editor.iface_value.SetValue(draft)
            click("整页预览")
            click("刷新预览")
            find("取消预览", "Button")
            wait("refreshed preview", lambda: root.descendants(title="刷新预览", control_type="Button"), timeout=25)
            pages("2 / 3", "3 / 4")
            click("逐段校对")
            assert find("译文校对", "Edit").iface_value.CurrentValue == draft

            click("保存校对")
            click("整页预览")
            click("逐段校对")
            click("整页预览")
            wait("reflowed translation", lambda: page_label("OfficeTranslatedPage") not in ("", "3 / 4"), timeout=25)
            reflowed = page_label("OfficeTranslatedPage")
            current, total = map(int, reflowed.split("/"))
            assert total > 4 and current == total - 1
            pages("2 / 3", reflowed)

            click("刷新预览")
            click("取消预览")
            find("已取消整页预览。", "Text")
            pages("2 / 3", reflowed)
            assert len(requests) == request_count
    assert document.read_bytes() == original_bytes
    print(f"Word preview ({theme}): verified linked scrolling and table pages 2/3 and 3/4, independent navigation, "
          "draft isolation, reflow, refresh and cancellation.")


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("template", type=Path)
    parser.add_argument("--screenshots", type=Path)
    parser.add_argument("--theme", choices=("light", "dark"), default="light")
    args = parser.parse_args()
    if not word_installed():
        print("Skipping the Word desktop preview test because Word is unavailable.")
        raise SystemExit(77)
    ctypes.windll.user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    with TemporaryDirectory(prefix="yiyi-office-preview-test-") as directory:
        document = Path(directory) / "材料评审.docx"
        pagination_fixture(args.template.resolve(strict=True), document)
        verify(args.executable.resolve(strict=True), document, args.screenshots, args.theme)
