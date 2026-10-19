#!/usr/bin/env python3
"""Regression check for an English clause copied into a Chinese translation."""

import json
from urllib.request import Request, urlopen


TEXT = ("A big pine key Florida finished first Overall, he completed the race "
        "in 41 minutes and one second and claimed the top spot in the midst of.")


def main() -> None:
    payload = json.dumps({"text": TEXT, "source": "en", "target": "zh"}).encode()
    request = Request("http://127.0.0.1:8767/v1/translate", payload,
                      {"Content-Type": "application/json"})
    with urlopen(request, timeout=90) as response:
        result = json.load(response)
    translation = result["translation"]
    assert result["translation_repaired"], result
    assert "finished first Overall" not in translation, result
    assert sum("\u4e00" <= c <= "\u9fff" for c in translation) >= 12, result
    print(json.dumps({"translation": translation,
                      "translation_ms": result["translation_ms"],
                      "translation_repaired": result["translation_repaired"]},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
