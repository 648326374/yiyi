import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def run(executable, fixtures, renderer_source):
    received = []
    release = threading.Event()

    class MockServer(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_POST(self):
            self.close_connection = True
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.append((self.path, body["messages"], self.headers["Authorization"]))
            index = len(received)
            event = {"choices": [{"delta": {"content": "未完成" if index == 1 else "重试成功"},
                                  "finish_reason": None if index == 1 else "stop"}]}
            payload = f"data: {json.dumps(event, ensure_ascii=False)}\n\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            if index == 1:
                self.send_header("Transfer-Encoding", "chunked")
                payload = f"{len(payload):x}\r\n".encode() + payload + b"\r\n"
            else:
                self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            self.wfile.flush()
            if index == 1:
                release.wait(timeout=10)

    server = ThreadingHTTPServer(("127.0.0.1", 0), MockServer)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for filename in ("sample.docx", "long.docx"):
                shutil.copyfile(fixtures / filename, directory / filename)
            (directory / "broken.docx").write_bytes(b"broken")
            tasks = directory / "profile" / "office-tasks"
            tasks.mkdir(parents=True)
            sha256 = hashlib.sha256((directory / "sample.docx").read_bytes()).hexdigest()
            cache = {
                "version": 1, "hash": sha256,
                "segments": [{"id": "word/document.xml:0", "source": "A practical guide", "translation": "旧译文",
                              "done": True, "kept": False, "failure": ""}],
                "draft": {"id": "word/document.xml:1", "text": "本机校对"},
                "provider": {"id": "legacy", "name": "Model", "model": "office-test", "baseUrl": base,
                             "protocol": "openai", "reasoning": ""},
                "source": "英语", "target": "简体中文", "tone": "自然", "fitText": True,
                "minimumFont": 12, "revision": 1, "exportedRevision": 0,
            }
            (tasks / f"{sha256}.json").write_text(json.dumps(cache, ensure_ascii=False), encoding="utf-8")
            renderer = directory / "Local renderer with spaces" / "soffice.exe"
            renderer.parent.mkdir()
            shutil.copyfile(renderer_source, renderer)
            preview_base = directory / "Temporary #25% 空格"
            preview_base.mkdir()
            env = dict(os.environ, TEMP=str(preview_base), TMP=str(preview_base))
            subprocess.run([executable, str(directory), base, str(renderer)], check=True, timeout=25, env=env)
            for task in tasks.glob("*.json"):
                assert "synthetic-key" not in task.read_text(encoding="utf-8")
        assert len(received) >= 5
        assert [messages[1]["content"] for _, messages, _ in received[:2]] == ["First item", "First item"]
        assert all(path == "/v1/chat/completions" and key == "Bearer synthetic-key" for path, _, key in received)
        assert all("英语 to 简体中文 in a 自然 style" in messages[0]["content"] for _, messages, _ in received[:2])
        assert all(len(messages[1]["content"].encode("utf-16-le")) // 2 <= 1000 for _, messages, _ in received)
    finally:
        release.set()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
