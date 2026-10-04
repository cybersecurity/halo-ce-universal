#!/usr/bin/env python3
"""Serve port/web locally for testing, at http://localhost:8000 by default.

Sends the cross-origin isolation headers (COOP/COEP) that a threaded
WebAssembly build needs for SharedArrayBuffer, so local testing matches what
the release host must send.
"""

import argparse
import functools
import http.server
from pathlib import Path


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".wasm": "application/wasm",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--directory", default=str(Path(__file__).resolve().parent))
    args = parser.parse_args()
    handler = functools.partial(Handler, directory=args.directory)
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"serving {args.directory} at http://localhost:{args.port}")
        server.serve_forever()


if __name__ == "__main__":
    main()
