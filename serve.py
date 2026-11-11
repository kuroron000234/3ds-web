#!/usr/bin/env python3
"""3DS-web の確認用サーバー。

スレッド（SharedArrayBuffer）を使うので COOP/COEP ヘッダを付けて配信する。
index.html は web/ から、ビルド成果物は /tmp/3ds/build/bin/Release から返す。
使い方: ./serve.py [port]
"""
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.join(HERE, "web")
BUILD_DIR = os.path.join(os.environ.get("WORK", "/tmp/3ds"), "build", "bin", "Release")
# 動作確認用の ROM / homebrew 置き場（リポジトリには入れない）
ROMS_DIR = os.path.join(os.environ.get("WORK", "/tmp/3ds"), "roms")


class Handler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        name = path.split("?", 1)[0].split("#", 1)[0].lstrip("/") or "index.html"
        name = os.path.basename(name)  # サブディレクトリは使わない
        # ビルド直後の成果物を、Home 側に退避したコピーより優先する
        for root in (BUILD_DIR, WEB_DIR, ROMS_DIR):
            candidate = os.path.join(root, name)
            if os.path.isfile(candidate):
                return candidate
        return os.path.join(WEB_DIR, name)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"http://localhost:{port}/")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
