"""Run on an unlocked Windows desktop with pywinauto and pywin32 installed."""

from argparse import ArgumentParser
from contextlib import closing, contextmanager
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
import subprocess
from tempfile import TemporaryDirectory
from threading import Event, Thread
import time

from pywinauto import Desktop, keyboard, mouse
import win32clipboard as clipboard
import win32con as wc
import win32gui as gui
import win32process


TRANSLATION = "书店九点开门。"
CANCELED_INPUT = "The service will wait for me to cancel."


def wait(label, probe):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if value := probe():
            return value
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {label}")


@contextmanager
def clipboard_access():
    clipboard.OpenClipboard()
    try:
        yield
    finally:
        clipboard.CloseClipboard()


@contextmanager
def paste_fixture():
    with clipboard_access():
        original = (clipboard.GetClipboardData(wc.CF_UNICODETEXT)
                    if clipboard.IsClipboardFormatAvailable(wc.CF_UNICODETEXT) else None)
    sequence = None

    def put(text):
        nonlocal sequence
        with clipboard_access():
            clipboard.EmptyClipboard()
            if text is not None:
                clipboard.SetClipboardText(text, wc.CF_UNICODETEXT)
            sequence = clipboard.GetClipboardSequenceNumber()

    try:
        yield put
    finally:
        if clipboard.GetClipboardSequenceNumber() == sequence:
            put(original)


@contextmanager
def fake_provider():
    requests = []
    release_cancelled = Event()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            assert self.path == "/v1/chat/completions"
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            messages = request["messages"]
            source = messages[-1]["content"]
            dictionary = messages[0]["content"].startswith("Act as a concise bilingual dictionary")
            requests.append((source, "dictionary" if dictionary else "translation", request["model"]))
            if source == CANCELED_INPUT:
                release_cancelled.wait(15)
                self.close_connection = True
                return
            result = "POS: n.\nTRANSLATION: 花园" if dictionary else TRANSLATION
            packet = {"choices": [{"delta": {"content": result}, "finish_reason": "stop"}]}
            body = f"data: {json.dumps(packet)}\r\n\r\ndata: [DONE]\r\n\r\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield requests, f"http://127.0.0.1:{server.server_port}/v1"
        finally:
            release_cancelled.set()
            server.shutdown()
            thread.join()


@contextmanager
def workbench(executable, settings):
    with TemporaryDirectory(prefix="yiyi-workbench-") as directory:
        profile = Path(directory)
        (profile / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
        system = Path(os.environ["SystemRoot"])
        env = dict(os.environ, PATH=f"{system / 'System32'};{system}")
        app = subprocess.Popen([executable, "--data-dir", profile, "--no-desktop"],
                               cwd=system, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
        window = None
        try:
            windows = Desktop(backend="uia")
            root = wait("workbench window", lambda: next(
                (item for item in windows.windows(process=app.pid)
                 if item.class_name() == "WinUIDesktopWin32WindowClass"), None))
            window = root.handle
            gui.SetWindowPos(window, wc.HWND_TOPMOST, 40, 40, 0, 0, wc.SWP_NOSIZE)
            try:
                gui.SetForegroundWindow(window)
            except gui.error:
                point = (70, 58)
                if gui.GetAncestor(gui.WindowFromPoint(point), wc.GA_ROOT) == window:
                    mouse.click(coords=point)
            wait("workbench foreground", lambda: gui.GetForegroundWindow() == window)

            def control(identifier):
                return wait(identifier, lambda: next(
                    (item for item in root.descendants() if item.element_info.automation_id == identifier), None))

            def keys(value):
                assert gui.GetForegroundWindow() == window
                keyboard.send_keys(value, with_spaces=True, pause=0.02)

            yield profile, root, control, keys
        finally:
            if app.poll() is None:
                if window and gui.IsWindow(window):
                    gui.PostMessage(window, wc.WM_CLOSE, 0, 0)
                try:
                    app.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait(timeout=3)


def screenshot(root, directory, name):
    if directory is None:
        return
    from PIL import ImageGrab

    directory.mkdir(parents=True, exist_ok=True)
    time.sleep(0.2)
    ctypes.windll.dwmapi.DwmFlush()
    bounds = gui.GetWindowRect(root.handle)
    left, top, right, bottom = bounds
    points = [(x, y) for x in (left + 16, (left + right) // 2, right - 16)
              for y in (top + 16, (top + bottom) // 2, bottom - 16)]

    def uncovered():
        return gui.GetForegroundWindow() == root.handle and all(
            gui.GetAncestor(gui.WindowFromPoint(point), wc.GA_ROOT) == root.handle for point in points)

    assert uncovered(), "The workbench is covered"
    image = ImageGrab.grab(bbox=bounds)
    assert uncovered(), "The workbench was covered during capture"
    image.save(directory / f"{name}.png")


def focus_editor(source):
    source.set_focus()
    wait("source editor keyboard focus", lambda: source.element_info.element.CurrentHasKeyboardFocus)


def verify(executable, screenshots):
    with fake_provider() as (requests, url), paste_fixture() as put:
        settings = {"selectedProviderId": "demo", "providers": [
            {"id": "demo", "name": "本机演示", "protocol": "openai", "model": "demo", "baseUrl": url},
            {"id": "alternate", "name": "备用演示", "protocol": "openai", "model": "demo-alt", "baseUrl": url},
        ]}
        expected = []

        def submitted(ui, source_text, kind="translation", model="demo"):
            expected.append((source_text, kind, model))
            wait("model request", lambda: len(requests) >= len(expected))
            assert requests == expected, (requests, expected)
            wait("saved result", lambda: ui("ResultFavorite").is_enabled())

        with workbench(executable, dict(settings, theme="light", mode="dictionary")) as (profile, root, ui, keys):
            source = ui("SourceText")
            result = ui("ResultText")
            assert source.iface_value.CurrentValue == result.iface_value.CurrentValue == ""
            assert not requests and not (profile / "history.sqlite").exists()
            assert source.rectangle().bottom < result.rectangle().top
            visible = [item for item in root.descendants() if item.is_visible()]
            assert not {"智能翻译", "文本翻译", "词典", "管理 AI 服务"}.intersection(item.window_text() for item in visible)
            assert not {"PageTitle", "ActiveProvider"}.intersection(item.element_info.automation_id for item in visible)
            language = ui("SourceLanguage").rectangle()
            capture = ui("ScreenshotButton").rectangle()
            assert language.top <= capture.mid_point().y <= language.bottom
            assert language.right < capture.left < ui("ClearButton").rectangle().left

            focus_editor(source)
            keys("The store opens at nine.")
            assert source.iface_value.CurrentValue == "The store opens at nine.", repr(source.iface_value.CurrentValue)
            assert not requests
            keys("^({ENTER})")
            submitted(ui, "The store opens at nine.")

            focus_editor(source)
            keys("^{HOME}{RIGHT 4}+({RIGHT 5})")
            put("bookshop")
            keys("^v")
            submitted(ui, "The bookshop opens at nine.")
            assert source.iface_value.CurrentValue == "The bookshop opens at nine."
            assert source.iface_text.GetSelection().GetElement(0).GetText(-1) == ""
            assert result.iface_value.CurrentValue == TRANSLATION
            screenshot(root, screenshots, "translate-light")

            put("  \r\n")
            ui("PasteButton").iface_invoke.Invoke()
            wait("blank clipboard message", lambda: ui("Status").is_visible())
            assert source.iface_value.CurrentValue == "The bookshop opens at nine."
            put("x" * 24000)
            ui("PasteButton").iface_invoke.Invoke()
            wait("limit message", lambda: any("24,000 个字符" in item.window_text() for item in ui("Status").descendants()))
            assert source.iface_value.CurrentValue == "The bookshop opens at nine."
            assert requests == expected

            source.iface_value.SetValue("x" * 24000)
            focus_editor(source)
            keys("^a")
            put(CANCELED_INPUT)
            ui("PasteButton").iface_invoke.Invoke()
            expected.append((CANCELED_INPUT, "translation", "demo"))
            wait("busy request", lambda: requests == expected)
            assert not ui("PasteButton").is_enabled()
            assert source.iface_value.CurrentIsReadOnly
            focus_editor(source)
            put("Additional text while busy.")
            keys("^v")
            assert source.iface_value.CurrentValue == CANCELED_INPUT

            def choose_alternate_provider():
                ui("SettingsItem").iface_selection_item.Select()
                use = wait("alternate AI service", lambda: root.descendants(
                    title="使用 备用演示", control_type="Button"))[0]
                use.iface_invoke.Invoke()

            choose_alternate_provider()
            wait("busy provider message", lambda: any("或先停止生成" in item.window_text() for item in ui("Status").descendants()))
            assert json.loads((profile / "settings.json").read_text(encoding="utf-8"))["selectedProviderId"] == "demo"
            ui("TranslateItem").iface_selection_item.Select()
            ui("CancelButton").iface_invoke.Invoke()
            wait("cancel complete", lambda: ui("TranslateButton").is_enabled())
            assert requests == expected
            with closing(sqlite3.connect(profile / "history.sqlite")) as db:
                assert db.execute("SELECT input, kind, model FROM records ORDER BY id").fetchall() == expected[:-1]

            ui("HistoryItem").iface_selection_item.Select()
            assert ui("PageTitle").window_text() == "查询历史"
            ui("LibraryList").descendants(control_type="ListItem")[0].iface_invoke.Invoke()
            wait("reopened history", lambda: ui("SourceText").iface_value.CurrentValue == "The bookshop opens at nine.")
            assert requests == expected
            print("Light workbench verified typing, Ctrl+Enter, Ctrl+V selection, limits, busy/cancel and reopening history.")

            choose_alternate_provider()
            wait("saved provider", lambda: json.loads((profile / "settings.json").read_text(encoding="utf-8"))[
                "selectedProviderId"] == "alternate")
            ui("TranslateItem").iface_selection_item.Select()
            assert ui("WorkbenchResultProvider").window_text() == "本机演示 · demo"
            ui("TranslateButton").iface_invoke.Invoke()
            submitted(ui, "The bookshop opens at nine.", model="demo-alt")
            assert ui("WorkbenchResultProvider").window_text() == "备用演示 · demo-alt"
            print("Provider switching in settings applies to the next request and preserves earlier result attribution.")

        with workbench(executable, dict(settings, theme="dark", mode="translation")) as (profile, root, ui, keys):
            dpi = ctypes.windll.user32.GetDpiForWindow(root.handle)
            gui.SetWindowPos(root.handle, 0, 0, 0, round(900 * dpi / 96), round(600 * dpi / 96),
                             wc.SWP_NOMOVE | wc.SWP_NOZORDER)
            source = ui("SourceText")
            put("garden")
            focus_editor(source)
            keys("+{F10}")
            windows = Desktop(backend="uia")

            def paste_item():
                return next((item for window in windows.windows(process=root.process_id())
                             for item in window.descendants(control_type="MenuItem")
                             if item.window_text() in ("粘贴", "Paste") and item.is_enabled()), None)

            wait("context menu paste", paste_item).iface_invoke.Invoke()
            submitted(ui, "garden", "dictionary")
            assert ui("WorkbenchResultKind").window_text() == "释义"
            assert source.rectangle().height() > 55 and ui("DictionaryScroll").rectangle().height() > 100
            assert source.rectangle().bottom < ui("DictionaryScroll").rectangle().top
            focus_editor(source)
            screenshot(root, screenshots, "translate-dark")

            keys("^a")
            screenshot(root, screenshots, "workbench-dark-selection")
            keys("The shop opens at noon.")
            ui("TranslateButton").iface_invoke.Invoke()
            submitted(ui, "The shop opens at noon.")
            print("Dark workbench at minimum size verified context menu paste, smart dictionary and translate icon.")

            def screenshot_overlay():
                handle = gui.FindWindow("YiyiWinUIScreenshotOverlay", None)
                if handle and win32process.GetWindowThreadProcessId(handle)[1] == root.process_id():
                    return handle

            ui("ScreenshotButton").iface_invoke.Invoke()
            overlay = wait("screenshot selection overlay", screenshot_overlay)
            gui.PostMessage(overlay, wc.WM_KEYDOWN, wc.VK_ESCAPE, 0)
            wait("screenshot cancel", lambda: not screenshot_overlay() and ui("ScreenshotButton").is_enabled())
            assert ui("SourceText").iface_value.CurrentValue == "The shop opens at noon."
            assert requests == expected
            print("Screenshot capture opens from the source toolbar and cancels with Escape.")

        with workbench(executable, {"theme": "light"}) as (profile, root, ui, keys):
            put("The original stays while setting up a service.")
            ui("PasteButton").iface_invoke.Invoke()
            wait("provider settings", lambda: ui("PageTitle").window_text() == "偏好设置")
            ui("TranslateItem").iface_selection_item.Select()
            assert ui("SourceText").iface_value.CurrentValue == "The original stays while setting up a service."
            assert requests == expected and not (profile / "history.sqlite").exists()
            print("Missing provider opens settings and keeps the pasted text.")


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--screenshots", type=Path, help="Capture the test results with Pillow")
    args = parser.parse_args()
    ctypes.windll.user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    verify(args.executable.resolve(strict=True), args.screenshots)
