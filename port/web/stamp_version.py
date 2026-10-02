"""Writes the web site's version.json (tools/web_build.py): a hash of the
site's files, which the service worker (site/sw.js) keeps each build under."""

import hashlib
import json
import sys


def main() -> None:
    output, inputs = sys.argv[1], sys.argv[2:]
    digest = hashlib.sha256()
    for path in sorted(inputs):
        with open(path, "rb") as file:
            digest.update(path.encode())
            digest.update(file.read())
    with open(output, "w", encoding="utf-8") as file:
        json.dump({"version": digest.hexdigest()[:16]}, file)


if __name__ == "__main__":
    main()
