import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


RESULT = "完整 🎋"
KEY = "private-test-secret"
PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jP1sAAAAASUVORK5CYII="


def json_bytes(value):
    return json.dumps(value, ensure_ascii=False).encode("utf-8")


def sse(value, kind=None):
    event = f"event: {kind}\r\n".encode() if kind else b""
    return event + b"data: " + json_bytes(value) + b"\r\n\r\n"


def cases(executable):
    completed_response = {
        "object": "response", "status": "completed", "error": None,
        "output": [
            {"type": "reasoning", "summary": [{"text": "internal reasoning"}]},
            {"type": "message", "role": "assistant", "content": [{"type": "output_text", "text": RESULT}]},
        ],
    }
    first_response = sse({"delta": "完"}, "response.output_text.delta")
    last_response = sse({"response": completed_response}, "response.completed")
    chat_result = sse({"choices": [{"delta": {"content": RESULT}, "finish_reason": "stop"}]})
    ollama_result = b"\n".join([
        json_bytes({"message": {"content": "完"}, "done": False}),
        json_bytes({"message": {"content": "整 🎋"}, "done": True, "error": None}),
    ])
    routes = {
        "plain": ("application/json", json_bytes(
            {"error": None, "choices": [{"message": {"content": RESULT}, "finish_reason": "stop"}]})),
        "sse": ("text/event-stream", chat_result),
        "responses": ("text/event-stream", first_response + last_response),
        "response-json": ("application/json", json_bytes(completed_response)),
        "ollama": ("application/x-ndjson", ollama_result),
        "ollama-json": ("application/json", ollama_result),
        "anthropic": ("text/event-stream",
                     sse({"delta": {"type": "text_delta", "text": RESULT}}, "content_block_delta") + sse({}, "message_stop")),
        "gemini": ("text/event-stream", sse({"candidates": [{
            "content": {"parts": [{"thought": True, "text": "internal reasoning"}, {"text": RESULT}]},
            "finishReason": "STOP",
        }]})),
        "incomplete": ("text/event-stream", first_response + b"data: [DONE]\n\n"),
        "large": ("application/json", json_bytes({"choices": [{"message": {"content": "x" * (2 * 1024 * 1024)}}]})),
        "401": ("application/json", json_bytes({"error": {"message": "invalid " + KEY}})),
        "vision-error": ("application/json", json_bytes({"error": {"message": "invalid data:image/png;base64," + PNG[:35]}})),
        "reasoning": ("application/json", json_bytes({"error": {"param": "reasoning.effort", "message": "unsupported " + KEY}})),
        "504": ("application/json", b'{"error":{"message":"gateway"}}'),
        "redirect": ("application/json", b""),
        "sink": ("text/event-stream", chat_result),
        "cancel-headers": ("text/event-stream", b""),
        "cancel-body": ("text/event-stream", b""),
        "early": ("text/event-stream", chat_result),
    }
    requests = []

    class MockServer(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_POST(self):
            self.close_connection = True
            route = self.path.split("/")[1]
            body = self.rfile.read(int(self.headers["Content-Length"]))
            requests.append((self.path, self.headers, json.loads(body)))
            if route == "cancel-headers":
                time.sleep(5)
                return
            kind, payload = routes[route]
            self.send_response({"401": 401, "vision-error": 400, "reasoning": 400, "504": 504, "redirect": 307}.get(route, 200))
            if route == "redirect":
                self.send_header("Location", "/sink/v1/chat/completions")
            self.send_header("Content-Type", kind)
            if route in ("early", "cancel-body"):
                self.send_header("Transfer-Encoding", "chunked")
            else:
                self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                if route == "responses":
                    self.send_parts(first_response, last_response, delay=0.2)
                elif route == "sse":
                    split = payload.index("🎋".encode()) + 1
                    self.send_parts(payload[:split], payload[split:])
                elif route == "early":
                    self.wfile.write(f"{len(payload):x}\r\n".encode() + payload + b"\r\n")
                    self.wfile.flush()
                    time.sleep(5)
                elif route == "cancel-body":
                    time.sleep(5)
                elif route == "reasoning":
                    self.send_parts(payload[:30], payload[30:])
                else:
                    self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

        def send_parts(self, first, last, delay=0.05):
            self.wfile.write(first)
            self.wfile.flush()
            time.sleep(delay)
            self.wfile.write(last)

    server = ThreadingHTTPServer(("127.0.0.1", 0), MockServer)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"

    def run(protocol, route, exit_code=0, contains=f"result {RESULT}", mode=None, path=None):
        command = [executable, protocol, origin + "/" + route]
        if mode:
            command.append(mode)
        result = subprocess.run(command, text=True, encoding="utf-8", capture_output=True, timeout=4)
        assert result.returncode == exit_code and contains in result.stdout, (route, result.returncode, result.stdout, result.stderr)
        assert KEY not in result.stdout, (route, result.stdout)
        if exit_code != 0:
            assert "result " not in result.stdout, (route, result.stdout)
        if path is not None:
            assert requests[-1][0] == "/" + route + path, requests[-1][0]
        print(route, flush=True)
        return result.stdout

    try:
        run("openai", "plain", path="/chat/completions")
        assert requests[-1][1]["Authorization"] == f"Bearer {KEY}"
        assert requests[-1][2] == {
            "messages": [{"content": "System prompt", "role": "system"}, {"content": "User text", "role": "user"}],
            "model": "test-model", "stream": True, "reasoning_effort": "medium",
        }
        run("openai", "sse", path="/chat/completions")
        events = run("responses", "responses", path="")
        assert "preview 完\n" in events and f"preview {RESULT}" in events, events
        assert requests[-1][2]["reasoning"] == {"effort": "medium"} and requests[-1][2]["store"] is False
        run("responses", "response-json")
        run("ollama", "ollama", path="/api/chat")
        assert requests[-1][2]["think"] is False and requests[-1][2]["options"]["num_predict"] == 4096
        run("ollama", "ollama-json", mode="probe")
        assert requests[-1][2]["options"]["num_predict"] == 64
        run("anthropic", "anthropic", path="/v1/messages")
        assert requests[-1][1]["x-api-key"] == KEY and requests[-1][1]["anthropic-version"] == "2023-06-01"
        run("gemini", "gemini", mode="probe", path="/models/test-model:streamGenerateContent?alt=sse")
        assert requests[-1][1]["x-goog-api-key"] == KEY and requests[-1][2]["generationConfig"]["maxOutputTokens"] == 4096
        data_url = "data:image/png;base64," + PNG
        run("openai", "plain", mode="vision")
        prompt, screenshot = requests[-1][2]["messages"]
        assert "Do not translate" in prompt["content"] and screenshot["content"][1] == {
            "type": "image_url", "image_url": {"url": data_url},
        }
        run("responses", "responses", mode="vision")
        assert requests[-1][2]["input"][0]["content"][1] == {"type": "input_image", "image_url": data_url}
        run("ollama", "ollama", mode="vision")
        assert requests[-1][2]["messages"][-1]["images"] == [PNG]
        run("anthropic", "anthropic", mode="vision")
        assert requests[-1][2]["messages"][0]["content"][0] == {
            "type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG},
        }
        run("gemini", "gemini", mode="vision")
        assert requests[-1][2]["contents"][0]["parts"][1]["inlineData"] == {"mimeType": "image/png", "data": PNG}
        error = run("openai", "vision-error", 3, "数据已隐藏", mode="vision")
        assert PNG[:35] not in error
        run("responses", "incomplete", 3, "提前结束")
        run("openai", "large", 3, "返回内容过长")
        run("openai", "401", 3, "已隐藏")
        run("responses", "reasoning", 3, "跟随服务")
        run("openai", "504", 3, "HTTP 504")
        run("openai", "redirect", 3, "重定向")
        assert not any(path.startswith("/sink/") for path, _, _ in requests)
        run("openai", "early")
        run("openai", "cancel-headers", 2, "cancelled", mode="cancel")
        run("openai", "cancel-body", 2, "cancelled", mode="cancel")
        previous_requests = len(requests)
        run("openai", "plain", 3, "换行", mode="invalid-key")
        run("openai", "plain?secret=invalid", 3, "查询参数")
        run("openai", "plain", 3, "图片过大", mode="oversize-vision")
        assert len(requests) == previous_requests
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    cases(sys.argv[1])
