#!/usr/bin/env python3
"""把 web/index.html（內嵌 web/vendor/*.js）壓縮成 gzip，產生韌體用的 web_ui.h。

用法：python3 tools/build_web.py
修改 web/ 底下的檔案後要重新執行，再重新燒錄韌體。
"""
import gzip
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
OUT = ROOT / "firmware" / "epaper_namecard" / "web_ui.h"


def bundle() -> str:
    html = (WEB / "index.html").read_text(encoding="utf-8")
    qr = (WEB / "vendor" / "qrcode.js").read_text(encoding="utf-8")
    marker = "<!--VENDOR_QRCODE-->"
    assert marker in html, f"{marker} not found in index.html"
    return html.replace(marker, "<script>\n" + qr + "\n</script>")


def main() -> None:
    data = gzip.compress(bundle().encode("utf-8"), compresslevel=9, mtime=0)
    lines = []
    for i in range(0, len(data), 20):
        lines.append("  " + ",".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    OUT.write_text(
        "// 自動產生，請勿手動修改。來源：web/index.html，產生方式：python3 tools/build_web.py\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        f"const size_t INDEX_HTML_GZ_LEN = {len(data)};\n"
        "const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n" + "\n".join(lines) + "\n};\n",
        encoding="utf-8",
    )
    print(f"wrote {OUT.relative_to(ROOT)} ({len(data)} bytes gzip)")


if __name__ == "__main__":
    main()
