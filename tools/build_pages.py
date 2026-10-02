#!/usr/bin/env python3
"""Package the verified browser runtime for a static host, without game data."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import setup_browser

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "dist/github-pages"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-repository", help="Optional public source repository URL for the deployment receipt")
    args = parser.parse_args()
    # Never copy build/web wholesale: it can contain local experiments.
    expected = set(setup_browser.FILES) | set(setup_browser.LOCAL_SCRIPTS) | {
        "download-provenance.json", ".nojekyll", "deployment.json",
    }
    OUTPUT.mkdir(parents=True, exist_ok=True)
    unexpected = {path.name for path in OUTPUT.iterdir()} - expected
    if unexpected:
        raise ValueError(f"Unexpected files in {OUTPUT}: {sorted(unexpected)}")
    with tempfile.TemporaryDirectory(prefix="halo-pages-") as temporary:
        staging = Path(temporary)
        setup_browser.main(staging)
        index = staging / "index.html"
        html = index.read_text().replace("<title>Halo</title>", "<title>Play Halo CE</title>")
        html = html.replace("Local copy of", "Self-hosted copy of")
        # All runtime URLs stay relative so /halo-ce-universal/ works.
        index.write_text(html)
        receipt_path = staging / "download-provenance.json"
        receipt = json.loads(receipt_path.read_text())
        receipt["local_index_sha256"] = setup_browser.digest(index.read_bytes())
        receipt["local_changes"].append("Static hosting title and attribution")
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
        (staging / ".nojekyll").touch()
        deployment = {
            "source_commit": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            "game_data_included": False,
            "files": {path.name: {"bytes": path.stat().st_size,
                                  "sha256": setup_browser.digest(path.read_bytes())}
                      for path in sorted(staging.iterdir())},
        }
        if args.source_repository:
            deployment["source_repository"] = args.source_repository
        (staging / "deployment.json").write_text(json.dumps(deployment, indent=2) + "\n")
        assert {path.name for path in staging.iterdir()} == expected
        for path in staging.iterdir():
            shutil.copyfile(path, OUTPUT / path.name)
    total = sum(path.stat().st_size for path in OUTPUT.iterdir())
    print(f"Static site: {OUTPUT} ({total:,} bytes; no maps or disc images)")


if __name__ == "__main__":
    main()
