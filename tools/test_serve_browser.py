"""Exercise local serving with generated map headers, never game assets."""
import contextlib
import http.client
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest import mock

import serve_browser


def synthetic_disc(path):
    sector = serve_browser.SECTOR
    data = bytearray(59 * sector)
    data[32 * sector:32 * sector + 20] = b"MICROSOFT*XBOX*MEDIA"
    struct.pack_into("<II", data, 32 * sector + 20, 33, sector)
    struct.pack_into("<HHIIBB", data, 33 * sector, 0, 0, 34, sector, 0x10, 4)
    data[33 * sector + 14:33 * sector + 18] = b"maps"
    offset = 0
    for index in range(24):
        name = b"ui.map" if index == 0 else f"test{index:02}.map".encode()
        next_offset = (offset + 14 + len(name) + 3) & ~3
        struct.pack_into("<HHIIBB", data, 34 * sector + offset,
                         0, next_offset // 4 if index < 23 else 0,
                         35 + index, sector, 0, len(name))
        start = 34 * sector + offset + 14
        data[start:start + len(name)] = name
        start = (35 + index) * sector
        data[start:start + 4] = b"daeh"
        struct.pack_into("<I", data, start + 4, 5)
        data[start + 0x40:start + 0x40 + 14] = b"01.10.12.2276\0"
        data[start + sector - 4:start + sector] = b"toof"
        offset = next_offset
    path.write_bytes(data)
    return bytes(data[35 * sector:36 * sector])


class BrowserServerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.web = self.root / "web"
        self.web.mkdir()
        (self.web / "index.html").write_text("local launcher")
        (self.web / "large.txt").write_text("x" * 4096)
        self.image = self.root / "synthetic.iso"
        self.ui_map = synthetic_disc(self.image)
        self.log_patch = mock.patch.object(serve_browser.Handler, "log_message", lambda *args: None)
        self.log_patch.start()
        self.addCleanup(self.log_patch.stop)

    @contextlib.contextmanager
    def running(self, image=None):
        # Keep one handler alive for HEAD -> GET to catch leaked transfer limits.
        with mock.patch.object(serve_browser.Handler, "protocol_version", "HTTP/1.1"):
            server = serve_browser.create_server(0, image, self.web)
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
            thread.start()
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            try:
                yield connection
            finally:
                connection.close()
                server.shutdown()
                server.server_close()
                thread.join()

    def test_launcher_without_disc(self):
        with self.running() as connection:
            connection.request("GET", "/")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b"local launcher")
            self.assertEqual(response.getheader("Cross-Origin-Opener-Policy"), "same-origin")
            self.assertEqual(response.getheader("Cache-Control"), "no-cache")
            self.assertEqual(response.getheader("Cross-Origin-Embedder-Policy"), "require-corp")
            connection.request("GET", "/assets/manifest.json")
            response = connection.getresponse()
            self.assertEqual(response.status, 404)
            self.assertIn(b"No disc image configured", response.read())

    def test_manifest_and_bounded_map_transfer(self):
        with self.running(self.image) as connection:
            connection.request("GET", "/assets/manifest.json")
            response = connection.getresponse()
            manifest = json.loads(response.read())
            self.assertEqual(len(manifest["files"]), 24)
            self.assertIn({"name": "maps/ui.map", "size": 2048}, manifest["files"])
            connection.request("GET", "/assets/maps/ui.map")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("Content-Length"), "2048")
            self.assertEqual(response.read(), self.ui_map)

    def test_head_does_not_limit_next_static_response(self):
        with self.running(self.image) as connection:
            connection.request("HEAD", "/assets/maps/ui.map")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("Content-Length"), "2048")
            self.assertEqual(response.read(), b"")
            connection.request("GET", "/large.txt")
            response = connection.getresponse()
            self.assertEqual(response.read(), b"x" * 4096)

    def test_bad_inputs_fail_before_listening(self):
        with mock.patch.object(serve_browser.http.server, "ThreadingHTTPServer") as constructor:
            with self.assertRaisesRegex(FileNotFoundError, "setup_browser.py"):
                serve_browser.create_server(0, web_root=self.root / "missing")
            self.image.write_bytes(b"not an Xbox filesystem")
            with self.assertRaisesRegex(ValueError, "no Xbox disc filesystem"):
                serve_browser.create_server(0, self.image, self.web)
            constructor.assert_not_called()


if __name__ == "__main__":
    unittest.main()
