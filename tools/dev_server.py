#!/usr/bin/env python3
"""在電腦上模擬 ESP 的 API，不接硬體也能試用/開發編輯器。

用法：python3 tools/dev_server.py [--port 8000] [--panel 128x296]
然後瀏覽器開 http://localhost:8000
「送到電子紙」收到的點陣圖會存成 tools/out/screen.png。
"""
import argparse
import base64
import json
import pathlib
import struct
import sys
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from build_web import bundle  # noqa: E402

OUT = pathlib.Path(__file__).resolve().parent / "out"
MAX_PAGES = 8
STATE = {"layouts": {}, "images": {}, "cur": 0, "ssid": ""}


def write_png(path, w, h, bits):
    """1-bit 點陣（1=黑）轉 PNG，依 rot 已是邏輯方向。"""
    rb = (w + 7) // 8
    raw = b""
    for y in range(h):
        row = bytes(bits[y * rb:(y + 1) * rb])
        raw += b"\x00" + bytes(b ^ 0xFF for b in row)  # PNG 1-bit：1=白
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 1, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    path.write_bytes(png)


class Handler(BaseHTTPRequestHandler):
    panel = (128, 296)

    def send(self, code, body, ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    @staticmethod
    def page(q):
        v = q.get("page")
        v = v[0] if isinstance(v, list) else v
        return int(v) if v is not None else STATE["cur"]

    def show(self, page):
        w, h, rot, bits = STATE["images"][page]
        OUT.mkdir(exist_ok=True)
        write_png(OUT / "screen.png", w, h, bits)
        STATE["cur"] = page
        print(f"[screen] page {page + 1}: {w}x{h} rot={rot} -> {OUT / 'screen.png'}")

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def do_GET(self):
        p = urlparse(self.path).path
        if p == "/":
            return self.send(200, bundle(), "text/html; charset=utf-8")
        if p == "/api/info":
            w, h = self.panel
            return self.send(200, json.dumps({
                "panel": f"模擬 {w}x{h}", "ver": "dev", "ap": "EPaper-Card-DEV", "apip": "192.168.4.1",
                "ssid": STATE["ssid"], "staip": "", "w": w, "h": h, "sleepMin": 0, "button": 0, "encoder": [3, 4, 5], "cur": STATE["cur"],
                "pages": [1 if i in STATE["images"] else 0 for i in range(MAX_PAGES)], "fsFree": 1400000}), "application/json")
        if p == "/api/layout":
            page = self.page(parse_qs(urlparse(self.path).query))
            if page not in STATE["layouts"]:
                return self.send(404, "no layout")
            return self.send(200, STATE["layouts"][page], "application/json")
        self.send(404, "not found")

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        data = self.body()
        if u.path == "/api/image":
            w, h, rot = int(q["w"]), int(q["h"]), int(q["rot"]) & 3
            ew, eh = (self.panel[1], self.panel[0]) if rot & 1 else self.panel
            if (w, h) != (ew, eh):
                return self.send(400, f"size mismatch: expect {ew}x{eh}")
            bits = base64.b64decode(data)
            if len(bits) != (w + 7) // 8 * h:
                return self.send(400, "bad bitmap")
            STATE["images"][self.page(q)] = (w, h, rot, bits)
            self.show(self.page(q))
            return self.send(200, "ok")
        if u.path == "/api/layout":
            # 取出 multipart 中的檔案內容
            boundary = self.headers["Content-Type"].split("boundary=")[1].encode()
            part = data.split(b"--" + boundary)[1]
            data = part.split(b"\r\n\r\n", 1)[1].rsplit(b"\r\n", 1)[0]
            STATE["layouts"][self.page(q)] = data
            print(f"[layout] page {self.page(q) + 1}: {len(data)} bytes")
            return self.send(200, "ok")
        if u.path == "/api/wifi":
            STATE["ssid"] = parse_qs(data.decode()).get("ssid", [""])[0]
            return self.send(200, "ok")
        if u.path == "/api/show":
            if self.page(q) not in STATE["images"]:
                return self.send(404, "no image on this page")
            self.show(self.page(q))
            return self.send(200, "ok")
        if u.path == "/api/next":
            for i in range(1, MAX_PAGES + 1):
                p = (STATE["cur"] + i) % MAX_PAGES
                if p in STATE["images"]:
                    self.show(p)
                    return self.send(200, "ok")
            return self.send(404, "no image")
        if u.path == "/api/delete":
            STATE["images"].pop(self.page(q), None)
            STATE["layouts"].pop(self.page(q), None)
            return self.send(200, "ok")
        if u.path in ("/api/netinfo", "/api/redraw", "/api/clear", "/api/reboot"):
            return self.send(200, "ok")
        self.send(404, "not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--panel", default="128x296", help="面板原生寬x高，例如 122x250、152x296")
    a = ap.parse_args()
    Handler.panel = tuple(int(x) for x in a.panel.split("x"))
    print(f"http://localhost:{a.port}  (panel {a.panel})")
    ThreadingHTTPServer(("", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
