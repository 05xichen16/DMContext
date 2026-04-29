"""
Simple file download server - Python 3.9+ compatible
Uses only standard library: http.server, mimetypes, urllib, json, os
"""

import json
import os
import mimetypes
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
from datetime import datetime

PORT = 8080
SERVE_DIR = "/opt/coremind/logs"

os.makedirs(SERVE_DIR, exist_ok=True)


def get_file_modified_time(filepath):
    """Return the file modification time as milliseconds and a local ISO string."""
    stat_result = os.stat(filepath)
    modified_ts = stat_result.st_mtime
    return int(modified_ts * 1000), datetime.fromtimestamp(modified_ts).isoformat(timespec="seconds")


class DownloadHandler(BaseHTTPRequestHandler):
    """Handle file download requests"""

    def log_message(self, format, *args):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {args[0]}")

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        # Health check
        if path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"status": "ok"}).encode())
            return

        # List files (recursive)
        if path in ("/", "/api/v1/log/files/list"):
            files = []
            for root, dirs, filenames in os.walk(SERVE_DIR):
                for name in filenames:
                    full = os.path.join(root, name)
                    rel = os.path.relpath(full, SERVE_DIR)
                    size = os.path.getsize(full)
                    modified_at_ms, modified_at = get_file_modified_time(full)
                    files.append({
                        "name": rel,
                        "size": size,
                        "modifiedAtMs": modified_at_ms,
                        "modifiedAt": modified_at,
                    })
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(files, indent=2).encode())
            return

        # Download specific file (supports subdirs like /api/v1/log/files/subdir/filename.txt)
        if path.startswith("/api/v1/log/files/"):
            relative_path = path[len("/api/v1/log/files/"):]  # strip "/api/v1/log/files/" prefix
            filepath = os.path.join(SERVE_DIR, relative_path)

            if not os.path.isfile(filepath):
                self.send_error(404, "File not found")
                return

            filename = os.path.basename(relative_path)
            size = os.path.getsize(filepath)
            content_type, _ = mimetypes.guess_type(filename)
            if not content_type:
                content_type = "application/octet-stream"

            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition", f"attachment; filename=\"{filename}\"")
            self.end_headers()

            with open(filepath, "rb") as f:
                while chunk := f.read(8192):
                    self.wfile.write(chunk)
            return

        self.send_error(404, "Not found")

    def do_POST(self):
        """Handle file upload"""
        if not self.path.startswith("/upload"):
            self.send_error(404, "Not found")
            return

        content_type = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in content_type:
            self.send_error(400, "Expected multipart/form-data")
            return

        boundary = None
        for part in content_type.split(";"):
            if "boundary" in part:
                boundary = part.split("=")[1].strip()
                break

        if not boundary:
            self.send_error(400, "No boundary found")
            return

        # Parse multipart body
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        parts = body.split(b"--" + boundary.encode())

        for part in parts:
            if b"filename=" not in part:
                continue
            # Extract filename from Content-Disposition
            header_end = part.find(b"\r\n\r\n")
            if header_end == -1:
                continue
            header = part[:header_end].decode("utf-8", errors="replace")
            content = part[header_end + 4:]

            for line in header.split("\r\n"):
                if "filename=" in line:
                    fname = line.split("filename=")[1].strip().strip('"')
                    break
            else:
                continue

            if not fname or fname.endswith('"'):
                continue

            # Remove trailing \r\n--
            if content.endswith(b"\r\n"):
                content = content[:-2]
            if content.endswith(b"--"):
                content = content[:-2]

            out_path = os.path.join(SERVE_DIR, os.path.basename(fname))
            with open(out_path, "wb") as f:
                f.write(content.strip(b"\r\n"))

            print(f"[{datetime.now().strftime('%H:%M:%S')}] Uploaded: {fname}")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"status": "uploaded"}).encode())


def run_server(port=PORT):
    server = HTTPServer(("", port), DownloadHandler)
    print(f"File server listening on http://0.0.0.0:{port}")
    print(f"Serving files from: {SERVE_DIR}")
    print("Endpoints:")
    print("  GET  /health                      - health check")
    print("  GET  /api/v1/log/files/list       - list all files (recursive)")
    print("  GET  /api/v1/log/files/<filename> - download file")
    print("  POST /upload                      - upload file")
    server.serve_forever()


if __name__ == "__main__":
    run_server()
