"""Check helper cache versioning using a tiny, verified offline runtime fixture."""
import contextlib
from html.parser import HTMLParser
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from urllib.parse import parse_qs, urljoin, urlsplit

import setup_browser


class Scripts(HTMLParser):
    def __init__(self, html):
        super().__init__()
        self.sources = []
        self.feed(html)

    def handle_starttag(self, tag, attributes):
        if tag == "script":
            source = dict(attributes).get("src")
            if source:
                self.sources.append(source)


class BrowserSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "dist/github-pages"
        cache = self.root / "build/browser-downloads"
        cache.mkdir(parents=True)
        scripts = self.root / "port/web"
        scripts.mkdir(parents=True)
        self.runtime = {name: f"fixture {name}".encode() for name in setup_browser.FILES}
        self.runtime["index.html"] = b'<html><head><title>Halo</title></head><body><script src="launcher.js"></script></body></html>'
        for name, data in self.runtime.items():
            (cache / name).write_bytes(data)
        for name in setup_browser.LOCAL_SCRIPTS:
            (scripts / name).write_bytes(b"const revision = 1;\n")
        for patch in (
            mock.patch.object(setup_browser, "ROOT", self.root),
            mock.patch.object(setup_browser, "FILES", {name: setup_browser.digest(data) for name, data in self.runtime.items()}),
            mock.patch.object(setup_browser.urllib.request, "urlopen", side_effect=AssertionError("test must stay offline")),
        ):
            patch.start()
            self.addCleanup(patch.stop)

    def install(self, output=None):
        output = output or self.output
        with contextlib.redirect_stdout(io.StringIO()):
            setup_browser.main(output)
        html = (output / "index.html").read_text()
        receipt = json.loads((output / "download-provenance.json").read_text())
        return html, receipt

    def helper_urls(self, html):
        return {urlsplit(source).path: source for source in Scripts(html).sources
                if urlsplit(source).path in setup_browser.LOCAL_SCRIPTS}

    def test_versions_match_installed_bytes_and_static_subpaths(self):
        html, receipt = self.install()
        sources = Scripts(html).sources
        self.assertEqual([urlsplit(source).path for source in sources],
                         [*setup_browser.LOCAL_SCRIPTS, "launcher.js"])
        for name, source in self.helper_urls(html).items():
            parsed = urlsplit(source)
            expected = setup_browser.digest((self.output / name).read_bytes())
            self.assertEqual(parse_qs(parsed.query), {"v": [expected]})
            self.assertFalse(parsed.scheme or parsed.netloc)
            self.assertEqual(urlsplit(urljoin("https://example.test/project/", source)).path,
                             f"/project/{name}")
            self.assertEqual(receipt["local_files"][name]["url"], source)
            self.assertEqual(receipt["local_files"][name]["sha256"], expected)
        # The static packager patches this pinned launcher tag separately.
        self.assertIn('<script src="launcher.js"></script>', html)
        self.assertEqual(receipt["local_index_sha256"], setup_browser.digest((self.output / "index.html").read_bytes()))

    def test_same_size_same_mtime_edit_changes_only_its_url(self):
        before, _ = self.install()
        changed = self.root / "port/web/stream-batch.js"
        original = changed.stat()
        changed.write_bytes(b"const revision = 2;\n")
        os.utime(changed, ns=(original.st_atime_ns, original.st_mtime_ns))
        after, receipt = self.install()
        before_urls, after_urls = self.helper_urls(before), self.helper_urls(after)
        for name in setup_browser.LOCAL_SCRIPTS:
            if name == changed.name:
                self.assertNotEqual(before_urls[name], after_urls[name])
            else:
                self.assertEqual(before_urls[name], after_urls[name])
        self.assertEqual((self.output / changed.name).read_bytes(), changed.read_bytes())
        self.assertEqual(receipt["local_files"][changed.name]["sha256"], setup_browser.digest(changed.read_bytes()))

    def test_unchanged_rebuild_and_relocation_preserve_urls(self):
        first, _ = self.install()
        second, _ = self.install()
        relocated, _ = self.install(self.root / "another-static-root")
        self.assertEqual(first, second)
        self.assertEqual(first, relocated)
        for name, original in self.runtime.items():
            if name != "index.html":
                self.assertEqual((self.output / name).read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
