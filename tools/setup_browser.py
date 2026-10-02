#!/usr/bin/env python3
"""Install the pinned Apollo browser release and local WebGL fixes."""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BASE = "https://html.itch.zone/html/19421784/"
FILES = {
    "index.html": "aaee0cbd99b36c0d0e35012f0b75c0389ee9d01658869220af619a2aa93faac4",
    "launcher.js": "58d30b68e287dd94523fb4712ba4059540cfa6d33c40c2332e74d74f0fd1c45d",
    "halo.js": "aeafc73007497ab44c0f2293824203ec281335edd0cf7275933443588bfc5bba",
    "halo.wasm": "aad4a196055760d5f082bf2af8580956325d513e4d51bc07fe8ab8f8de9cd636",
    "halo-asyncify.js": "f082f25e3bba34a1d8f67716feb59071f6f02a110da183b9c83d8d2ae9bb2c15",
    "halo-asyncify.wasm": "77edefd9f55f564d69e9b16491a11ab173c6123f830663873a5cbe877a745696",
    "sw.js": "1b3d773d5f575cc45276d1b9aeefa6b62106023059457c6c8c31a2d87ef49430",
}
LOCAL_SCRIPTS = ("opaque-canvas.js", "stream-batch.js", "replay-cache.js",
                 "vertex-state-cache.js", "performance.js")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main(output=None):
    output = Path(output) if output is not None else ROOT / "build/web"
    cache = ROOT / "build/browser-downloads"
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    receipt = {"source_page": "https://bnunu.itch.io/apollobeta", "source_base": BASE, "files": {}}
    for name, expected in FILES.items():
        data = None
        for candidate in (cache / name, output / name):
            if candidate.exists():
                candidate_data = candidate.read_bytes()
                if digest(candidate_data) == expected:
                    data = candidate_data
                    break
        if data is None:
            with urllib.request.urlopen(BASE + name, timeout=60) as response:
                data = response.read()
        if digest(data) != expected:
            raise ValueError(f"Unexpected SHA-256 for {name}; refusing to install a changed release")
        (cache / name).write_bytes(data)
        (output / name).write_bytes(data)
        receipt["files"][name] = {"url": BASE + name, "bytes": len(data), "sha256": expected}
        print(f"Verified {name}")

    index = output / "index.html"
    html = index.read_text()
    html = html.replace('<script defer src="https://static.itch.io/htmlgame.js" type="text/javascript"></script>', '')
    html = html.replace('href="guide.html"', f'href="{BASE}guide.html"')
    html = html.replace('<p class="hint-big">Finding Halo NTSC .XISO is easy. Google could be your friend :)</p>',
        '<p class="help">Local copy of <a href="https://bnunu.itch.io/apollobeta" target="_blank" rel="noopener">bnunu’s Apollo browser beta</a>.</p>')
    # Hash the exact bytes installed, not timestamps: rapid edits and static
    # hosting caches can otherwise reuse an older helper under the same URL.
    local_files = {}
    for name in LOCAL_SCRIPTS:
        data = (ROOT / "port/web" / name).read_bytes()
        (output / name).write_bytes(data)
        sha256 = digest(data)
        local_files[name] = {"bytes": len(data), "sha256": sha256,
                             "url": f"{name}?v={sha256}"}
    scripts = "\n".join(f'<script src="{local_files[name]["url"]}"></script>'
                        for name in LOCAL_SCRIPTS)
    html = html.replace('<script src="launcher.js"></script>',
                        scripts + '\n<script src="launcher.js"></script>')
    index.write_text(html)
    receipt["local_changes"] = [
        "Opaque WebGL canvas", "Default stream upload batching with batch_streams=0 opt-out",
        "Render-state and vertex-state caches with independent opt-outs",
        "FPS display", "Removed itch.io host script",
        "Publisher credit and guide link", "Content-versioned local scripts",
    ]
    receipt["local_files"] = local_files
    receipt["installed_engine"] = {
        name: digest((output / name).read_bytes())
        for name in ("halo.js", "halo.wasm", "halo-asyncify.js", "halo-asyncify.wasm")
    }
    receipt["local_index_sha256"] = digest(index.read_bytes())
    receipt["opaque_canvas_sha256"] = digest((output / "opaque-canvas.js").read_bytes())
    (output / "download-provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("Browser build ready. Run python3 tools/serve_browser.py, then open http://127.0.0.1:8767/")


if __name__ == "__main__":
    main()
