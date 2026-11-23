import json
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def run(executable):
    requests = []
    release = threading.Event()

    class MockServer(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_POST(self):
            self.close_connection = True
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, body["messages"], self.headers["Authorization"]))
            index = len(requests)
            if index == 2:
                payload = b'{"error":{"message":"simulated failure"}}'
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
            else:
                text = {1: "第一", 3: "临时内容", 4: "第二"}[index]
                event = {"choices": [{"delta": {"content": text}, "finish_reason": None if index == 3 else "stop"}]}
                payload = f"data: {json.dumps(event, ensure_ascii=False)}\n\n".encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                if index == 3:
                    self.send_header("Transfer-Encoding", "chunked")
                    payload = f"{len(payload):x}\r\n".encode() + payload + b"\r\n"
                else:
                    self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            self.wfile.flush()
            if index == 3:
                release.wait(timeout=10)

    server = ThreadingHTTPServer(("127.0.0.1", 0), MockServer)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "input.txt"
            source.write_bytes(b"  First \r\n\r\n\tSecond\r\n")
            subprocess.run([executable, str(source), f"http://127.0.0.1:{server.server_port}"], check=True, timeout=20)
        assert [messages[1]["content"] for _, messages, _ in requests] == ["First", "Second", "Second", "Second"]
        assert all(path == "/v1/chat/completions" and key == "Bearer private-test-secret" for path, _, key in requests)
        assert len({messages[0]["content"] for _, messages, _ in requests}) == 1
        assert "英语 into 简体中文 in a 自然 style" in requests[0][1][0]["content"]
    finally:
        release.set()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    run(sys.argv[1])
