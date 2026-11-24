"""Exercise the DOCX preview on an unlocked Windows desktop with local Word."""

from argparse import ArgumentParser
from pathlib import Path
import winreg

from workbench_smoke import ctypes, fake_provider, screenshot, wait, workbench


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


def verify(executable, document, screenshots):
    original_bytes = document.read_bytes()
    translations = {
        "A practical guide": "实用指南",
        "Keep the important meaning when translating.": "翻译时保留关键信息。",
        "First item": "第一项",
        "Merged header": "合并表头",
        "Name": "名称",
        "Description": "说明",
        "Yiyi": "译意",
        "A local translation tool": "本地翻译工具",
        "Before tab": "制表符前",
        "After tab": "制表符后",
        "After break": "换行之后",
        "Final paragraph with numbers 2026 and punctuation.": "末段保留数字 2026 和标点。",
        "Read the footnote": "阅读脚注",
        "Read the endnote": "阅读尾注",
        "Internal report": "内部报告",
        "A useful endnote.": "一条有用的尾注。",
    }
    with fake_provider(lambda source: translations.get(source, source)) as (requests, url):
        settings = {"theme": "light", "selectedProviderId": "preview-test", "providers": [
            {"id": "preview-test", "name": "本机演示", "model": "preview", "protocol": "openai", "baseUrl": url}
        ]}
        with workbench(executable, settings, "--document", document) as (_, root, ui, _):
            def find(title, kind=None):
                return wait(title, lambda: root.descendants(title=title, control_type=kind))[0]

            def click(title):
                find(title, "Button").iface_invoke.Invoke()

            def page_label(identifier):
                return ui(identifier).window_text()

            def ready(title):
                return any(control.rectangle().height() > 20 for control in
                           root.descendants(title=title, control_type="Image"))

            def reading(title):
                wait(title, lambda: ready(title), timeout=25)

            reading("原文整页预览")
            source_page = page_label("OfficeOriginalPage")
            assert source_page.startswith("1 / ")
            assert not ready("译文整页预览") and not requests

            click("开始翻译")
            reading("译文整页预览")
            translated_page = page_label("OfficeTranslatedPage")
            assert requests and translated_page.startswith("1 / ")
            original = find("原文整页预览", "Image").rectangle()
            assert find("译文整页预览", "Image").rectangle().left > original.left
            assert find("另存译文", "Button").rectangle().bottom <= original.top
            assert find("重新翻译", "Button").rectangle().bottom <= original.top
            if screenshots:
                screenshot(root, screenshots, "docx-preview")
            width = find("原文整页预览", "Image").rectangle().width()
            zoom = find("预览缩放", "ComboBox")
            zoom.select("适合整页")
            wait("fit the full page", lambda: find("原文整页预览", "Image").rectangle().width() < width)
            zoom.select("适合栏宽")

            request_count = len(requests)
            click("逐段校对")
            segments = find("文档段落", "List")
            segments.descendants(control_type="ListItem")[0].iface_selection_item.Select()
            editor = find("译文校对", "Edit")
            draft = "这是用于检查分页的校对译文。" * 180
            editor.iface_value.SetValue(draft)
            click("整页预览")
            click("刷新预览")
            find("取消预览", "Button")
            wait("refreshed preview", lambda: root.descendants(title="刷新预览", control_type="Button"), timeout=25)
            reading("译文整页预览")
            assert page_label("OfficeTranslatedPage") == translated_page
            click("逐段校对")
            assert find("译文校对", "Edit").iface_value.CurrentValue == draft

            click("保存校对")
            click("整页预览")
            wait("pagination after committing the longer translation",
                 lambda: page_label("OfficeTranslatedPage") != translated_page and ready("译文整页预览"), timeout=25)
            left = page_label("OfficeOriginalPage")
            assert left == source_page
            click("译文下一页")
            wait("translated second page", lambda: page_label("OfficeTranslatedPage").startswith("2 / "))
            assert page_label("OfficeOriginalPage") == left

            click("刷新预览")
            click("取消预览")
            find("已取消整页预览。", "Text")
            assert page_label("OfficeOriginalPage") == left and len(requests) == request_count
    assert document.read_bytes() == original_bytes
    print("Word preview verified local source, two rendered panes, separate pagination, draft isolation, refresh and cancellation.")


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("document", type=Path)
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    if not word_installed():
        print("Skipping the Word desktop preview test because Word is unavailable.")
        raise SystemExit(77)
    ctypes.windll.user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    verify(args.executable.resolve(strict=True), args.document.resolve(strict=True), args.screenshots)
