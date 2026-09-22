#!/usr/bin/env python3
"""HTTP/multipart transport for the production NativeFontWebHost executable.

python test/native_text/font_web_http.py --driver build/test/native_text/NativeFontWebHost.exe --root build/font-web-sd
The driver owns every font policy, response, registry and filesystem operation.
"""

import argparse
import email.parser
import email.policy
import http.server
import json
from pathlib import Path
import subprocess
import urllib.parse


class Driver:
    def __init__(self, executable, root):
        self.process = subprocess.Popen(
            [str(Path(executable).resolve()), str(Path(root).resolve())],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            encoding="utf-8", bufsize=1,
        )

    def call(self, op, **arguments):
        self.process.stdin.write(json.dumps({"op": op, **arguments}) + "\n")
        self.process.stdin.flush()
        result = self.process.stdout.readline()
        if not result:
            raise RuntimeError("NativeFontWebHost exited; inspect its stderr")
        return json.loads(result)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)


class Multipart:
    """Bounded multipart decoding only; never interprets font metadata/bytes."""

    def __init__(self, stream, length, boundary):
        self.stream = stream
        self.remaining = length
        self.buffer = b""
        self.boundary = boundary

    def read(self):
        if self.remaining <= 0:
            raise ValueError("Incomplete multipart request")
        chunk = self.stream.read(min(16384, self.remaining))
        if not chunk:
            raise ValueError("Disconnected multipart request")
        self.remaining -= len(chunk)
        self.buffer += chunk

    def until(self, separator, limit):
        while True:
            position = self.buffer.find(separator)
            if position >= 0:
                if position > limit:
                    raise ValueError("Multipart header too large")
                value, self.buffer = self.buffer[:position], self.buffer[position + len(separator):]
                return value
            if len(self.buffer) > limit:
                raise ValueError("Multipart header too large")
            self.read()

    def take(self, count):
        while len(self.buffer) < count:
            self.read()
        value, self.buffer = self.buffer[:count], self.buffer[count:]
        return value

    def chunks(self):
        delimiter = b"\r\n--" + self.boundary
        while True:
            position = self.buffer.find(delimiter)
            if position >= 0:
                # A boundary-like byte sequence is data unless followed by the
                # MIME line ending or the terminal '--'. Fonts are binary.
                while len(self.buffer) < position + len(delimiter) + 2:
                    self.read()
                suffix = self.buffer[position + len(delimiter):position + len(delimiter) + 2]
                if suffix in (b"\r\n", b"--"):
                    yield self.buffer[:position]
                    self.buffer = self.buffer[position + len(delimiter):]
                    return
                yield self.buffer[:position + 2]
                self.buffer = self.buffer[position + 2:]
                continue
            available = len(self.buffer) - len(delimiter) - 2
            if available > 0:
                yield self.buffer[:available]
                self.buffer = self.buffer[available:]
            self.read()

    def stream_files(self, driver):
        if self.until(b"\r\n", 256) != b"--" + self.boundary:
            raise ValueError("Missing multipart boundary")
        while True:
            raw_headers = self.until(b"\r\n\r\n", 8192)
            headers = email.parser.BytesParser(policy=email.policy.default).parsebytes(raw_headers + b"\r\n\r\n")
            filename = headers.get_filename()
            if filename is None or headers.get_content_disposition() != "form-data":
                raise ValueError("Expected a multipart file")
            driver.call("file", name=filename)
            for chunk in self.chunks():
                if chunk:
                    driver.call("chunk", hex=chunk.hex())
            driver.call("end")
            ending = self.take(2)
            if ending == b"--":
                # Consume the transport framing completely before committing.
                while self.remaining:
                    self.read()
                    if len(self.buffer) > 8192:
                        raise ValueError("Unexpected multipart epilogue")
                if self.buffer not in (b"", b"\r\n"):
                    raise ValueError("Unexpected multipart epilogue")
                return
            if ending != b"\r\n":
                raise ValueError("Invalid multipart boundary")


class Handler(http.server.BaseHTTPRequestHandler):
    def send_driver(self, result):
        body = result["body"].encode("utf-8")
        self.send_response(result["status"])
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path in ("/", "/fonts"):
            body = self.server.html.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/fonts":
            self.send_driver(self.server.driver.call("list"))
        else:
            self.send_error(404)

    def do_POST(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path not in ("/api/fonts/upload", "/api/fonts/delete"):
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            if length < 0:
                raise ValueError("Content-Length required")
            self.connection.settimeout(30)
            if url.path == "/api/fonts/delete":
                if length > 8192:
                    self.send_error(413)
                    return
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError("Incomplete request")
                self.send_driver(self.server.driver.call("delete", body=body.decode("utf-8")))
                return
            query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
            self.server.driver.call("begin", manifest=query.get("manifest", [""])[0])
            content_type = email.message.Message()
            content_type["Content-Type"] = self.headers.get("Content-Type", "")
            boundary = content_type.get_boundary()
            if content_type.get_content_type() != "multipart/form-data" or not boundary or len(boundary) > 200:
                raise ValueError("Expected multipart/form-data")
            Multipart(self.rfile, length, boundary.encode("ascii")).stream_files(self.server.driver)
            self.send_driver(self.server.driver.call("finish"))
        except (ValueError, UnicodeError, TimeoutError, ConnectionError, OSError):
            result = self.server.driver.call("abort")
            self.close_connection = True
            try:
                self.send_driver(result)
            except (ConnectionError, OSError):
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--html", type=Path, default=Path(__file__).resolve().parents[2] / "src/network/html/FontsPage.html")
    args = parser.parse_args()
    driver = Driver(args.driver, args.root)
    server = http.server.HTTPServer((args.host, args.port), Handler)
    server.driver, server.html = driver, args.html
    # Actual list response proves the engine and production handler are ready.
    if driver.call("list")["status"] != 200:
        driver.close()
        raise RuntimeError("Font handler failed to initialize")
    print(f"Font web host ready: http://{args.host}:{args.port}/fonts", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        driver.close()


if __name__ == "__main__":
    main()
