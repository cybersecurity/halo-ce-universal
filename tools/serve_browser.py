#!/usr/bin/env python3
"""Serve bnunu's published browser build and local Xbox map data on loopback."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import struct
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 2048


def disc_maps(image):
    with image.open("rb") as disc:
        for base in (0, 0x18300000, 0x0FD90000, 0x02080000):
            disc.seek(base + 32 * SECTOR)
            header = disc.read(28)
            if header[:20] == b"MICROSOFT*XBOX*MEDIA":
                break
        else:
            raise ValueError("The file has no Xbox disc filesystem")

        def directory(sector, size):
            disc.seek(base + sector * SECTOR)
            data = disc.read(size)
            pending, seen, entries = [0], set(), []
            while pending:
                offset = pending.pop()
                if offset in seen or offset + 14 > len(data):
                    continue
                seen.add(offset)
                left, right, start, length, flags, count = struct.unpack_from("<HHIIBB", data, offset)
                if left == 0xFFFF:
                    continue
                name = data[offset + 14:offset + 14 + count].decode("ascii").lower()
                entries.append((name, start, length, flags))
                pending.extend(child * 4 for child in (left, right) if child)
            return entries

        root = directory(*struct.unpack_from("<II", header, 20))
        folder = next((entry for entry in root if entry[0] == "maps" and entry[3] & 0x10), None)
        if folder is None:
            raise ValueError("The disc image has no maps directory")
        maps = {}
        for name, sector, size, flags in directory(folder[1], folder[2]):
            if flags & 0x10 or not name.endswith(".map") or "/" in name or "\\" in name:
                continue
            offset = base + sector * SECTOR
            if offset + size > image.stat().st_size:
                raise ValueError(f"Truncated disc image: {name}")
            disc.seek(offset)
            map_header = disc.read(0x800)
            build = map_header[0x40:0x60].split(b"\0", 1)[0].decode("ascii")
            if (map_header[:4] != b"daeh" or map_header[-4:] != b"toof"
                    or struct.unpack_from("<I", map_header, 4)[0] != 5
                    or build not in ("01.10.12.2276", "01.01.14.2342")):
                raise ValueError(f"Unsupported map: {name} ({build})")
            maps["/assets/maps/" + name] = (offset, size)
        if len(maps) != 24 or "/assets/maps/ui.map" not in maps:
            raise ValueError(f"Expected 24 Halo maps, found {len(maps)}")
        return maps


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

    def end_headers(self):
        # Revalidate local builds so Chrome does not reuse a stale launcher.
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(403)

    def send_head(self):
        self.remaining = None
        path = urlsplit(self.path).path
        if path.startswith("/assets/") and self.server.image is None:
            self.send_error(404, "No disc image configured; import your ISO in the launcher or use --image")
            return None
        if path == "/assets/manifest.json":
            import io
            data = json.dumps({"files": [
                {"name": name.removeprefix("/assets/"), "size": size}
                for name, (_, size) in sorted(self.server.maps.items())
            ]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            return io.BytesIO(data)
        if path.startswith("/assets/"):
            if path not in self.server.maps:
                self.send_error(404)
                return None
            offset, size = self.server.maps[path]
            disc = self.server.image.open("rb")
            disc.seek(offset)
            self.remaining = size
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            return disc
        return super().send_head()

    def copyfile(self, source, outputfile):
        remaining = getattr(self, "remaining", None)
        if remaining is None:
            return super().copyfile(source, outputfile)
        while remaining:
            data = source.read(min(1024 * 1024, remaining))
            if not data:
                raise OSError("Disc image ended during map transfer")
            outputfile.write(data)
            remaining -= len(data)


def create_server(port, image=None, web_root=None):
    """Validate local inputs before opening the loopback listener."""
    web_root = Path(web_root or ROOT / "build/web").resolve()
    if not (web_root / "index.html").is_file():
        raise FileNotFoundError("Browser build is missing. Run python3 tools/setup_browser.py first.")
    image = Path(image).expanduser().resolve() if image is not None else None
    maps = disc_maps(image) if image is not None else {}
    handler = functools.partial(Handler, directory=str(web_root))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.image = image
    server.maps = maps
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, help="Optional Xbox ISO to serve maps from; otherwise import files in the browser")
    parser.add_argument("--port", type=int, default=8767)
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    try:
        server = create_server(args.port, args.image)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    if server.image is not None:
        print(f"Validated {len(server.maps)} maps from {server.image}", flush=True)
    else:
        print("Import your ISO or maps with the browser launcher, or reuse its stored maps.", flush=True)
    suffix = "?data=/assets/" if server.image is not None else ""
    print(f"Open http://127.0.0.1:{server.server_port}/{suffix} in Chrome, then press Play.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
