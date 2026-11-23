import contextlib
import hashlib
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from pdf_merge_cases import create_pdf, inspect, object_record, stream


def make_pdf(path):
    create_pdf(path, [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] /MediaBox [0 0 300 220] "
        b"/Resources << /Font << /F1 7 0 R >> >> >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 5 0 R /Annots [8 0 R] >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 6 0 R >>",
        stream(b"BT /F1 16 Tf 24 190 Td (First page) Tj ET"),
        stream(b"BT /F1 16 Tf 24 190 Td (Second page) Tj ET"),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Annot /Subtype /Link /Rect [24 32 70 50] /Dest [4 0 R /Fit] /P 3 0 R >>",
    ])
    return path.read_bytes()


def check_export(executable, qpdf, root, environment):
    worker = pathlib.Path(executable).resolve().with_name("yiyi_pdf_worker.exe")
    request = root / "worker-request.json"
    response = root / "worker-response.json"

    def invoke(source, action, **fields):
        request.write_text(json.dumps({"action": action, "source": str(source), **fields}), encoding="utf-8")
        subprocess.run([worker, request, response], check=True, capture_output=True, env=environment, timeout=8)
        result = json.loads(response.read_text(encoding="utf-8"))
        assert "error" not in result, result
        return result

    for name, expected in (("完整译稿.pdf", ("第一页", "第二页")), ("部分译稿.pdf", ("第一页", "Second page"))):
        output = root / name
        manifest = invoke(output, "info")
        assert manifest["pageCount"] == 2
        for page, text in enumerate(expected):
            actual = invoke(output, "extractPageInput", document=manifest, page=page)["text"]
            assert actual.strip() == text, actual
        pdf = inspect(qpdf, output, environment)
        first = object_record(pdf, pdf["pages"][0]["object"])["value"]
        link = object_record(pdf, first["/Annots"][0])["value"]
        assert link["/Subtype"] == "/Link" and link["/Dest"][0] == pdf["pages"][1]["object"]

    report = (root / "部分译稿.pdf.issues.txt").read_text(encoding="utf-8-sig")
    assert "第 2 页" in report and "原文已修改" in report


def main(executable, output_root, qpdf):
    seen = []
    requests = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            source = request["messages"][-1]["content"]
            with lock:
                seen.append(source)
                requests.append((time.monotonic(), request["messages"][0]["content"]))
                count = seen.count(source)
            if source == "Second page" and count == 1:
                time.sleep(2)
            results = {"First page": "第一页" if count == 1 else "<|im_start|>", "Second page": "第二页"}
            response = json.dumps({"choices": [{"message": {"content": results[source]}, "finish_reason": "stop"}]}).encode()
            try:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)
                if source == "First page" and count == 5:
                    self.wfile.flush()
                    (root / "retry-sent").touch()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

        def log_message(self, *_):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server, tempfile.TemporaryDirectory(
        dir=output_root, prefix="pdf-workflow-"
    ) as directory:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            root = pathlib.Path(directory).resolve()
            source = root / "PDF 测试.pdf"
            original = make_pdf(source)
            profile = root / "profile"
            digest = hashlib.sha256(original).hexdigest()
            legacy_cache = profile / "legacy" / "pdf-tasks"
            legacy_cache.mkdir(parents=True)
            (legacy_cache / f"{digest}.json").write_text(json.dumps({
                "version": 1, "hash": digest,
                "translations": [{"id": "p0-b0", "source": "First page", "translation": "旧版第一页"}],
            }), encoding="utf-8")
            system = pathlib.Path(os.environ["SystemRoot"])
            environment = {**os.environ, "PATH": f"{system / 'System32'};{system}"}
            subprocess.run(
                [executable, source, profile, f"http://127.0.0.1:{server.server_port}"],
                check=True,
                env=environment,
                timeout=25,
            )
            assert source.read_bytes() == original
            assert (root / "源文件硬链接.pdf").read_bytes() == original
            check_export(executable, qpdf, root, environment)
            assert seen == ["First page", "Second page", "Second page", "First page", "First page", "First page", "First page"], seen
            assert 'Document opening for topic context' in requests[0][1] and '["First page"]' in requests[2][1]
            for _, prompt in requests[3:]:
                assert "one table cell" in prompt and 'Inherited column heading' in prompt, prompt
            assert "previous response failed validation" not in requests[3][1]
            assert "previous response failed validation" in requests[4][1] and "previous response failed validation" in requests[5][1]
            assert requests[4][0] - requests[3][0] >= 0.95
            assert requests[5][0] - requests[4][0] >= 1.95
            with contextlib.closing(sqlite3.connect(profile / "pdf-tasks" / f"{digest}.sqlite")) as db:
                metadata = db.execute("SELECT data FROM metadata WHERE id=1").fetchone()[0]
            assert b"fixture-secret" not in metadata
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main(*sys.argv[1:])
