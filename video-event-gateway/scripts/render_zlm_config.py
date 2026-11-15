#!/usr/bin/env python3
"""Render a small, LAN-bound ZLMediaKit runtime config from its upstream sample."""

import argparse
import os
from pathlib import Path
import re
import secrets


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--listen-ip", required=True)
    parser.add_argument("--http-port", type=int, default=18089)
    parser.add_argument("--rtsp-port", type=int, default=8554)
    parser.add_argument("--rtmp-port", type=int, default=1935)
    parser.add_argument("--enable-hls", action="store_true")
    parser.add_argument("--http-root", type=Path,
                        help="Absolute HTTP/HLS root, e.g. a user tmpfs directory")
    args = parser.parse_args()
    for port in (args.http_port, args.rtsp_port, args.rtmp_port):
        if not 1 <= port <= 65535:
            parser.error("ports must be in the range 1..65535")
    if args.output.exists():
        parser.error(f"refusing to overwrite {args.output}")
    if args.http_root is not None and not args.http_root.is_absolute():
        parser.error("--http-root must be an absolute path")

    values = {
        ("api", "apiDebug"): "0",
        ("api", "secret"): secrets.token_urlsafe(32),
        ("general", "listen_ip"): args.listen_ip,
        ("protocol", "enable_hls"): "1" if args.enable_hls else "0",
        ("protocol", "enable_hls_fmp4"): "0",
        ("protocol", "enable_ts"): "0",
        ("protocol", "enable_fmp4"): "0",
        ("http", "port"): str(args.http_port),
        ("http", "sslport"): "0",
        ("rtsp", "port"): str(args.rtsp_port),
        ("rtsp", "sslport"): "0",
        ("rtmp", "port"): str(args.rtmp_port),
        ("rtmp", "sslport"): "0",
        ("rtp_proxy", "port"): "0",
        ("onvif", "port"): "0",
    }
    if args.http_root is not None:
        values[("http", "rootPath")] = str(args.http_root)
        if args.enable_hls:
            values[("protocol", "hls_save_path")] = str(args.http_root)
    seen = set()
    section = ""
    rendered = []
    for line in args.sample.read_text(encoding="utf-8").splitlines(keepends=True):
        match = re.match(r"^\[([^]]+)\]", line)
        if match:
            section = match.group(1)
        else:
            match = re.match(r"^([A-Za-z][A-Za-z0-9_]*)=", line)
            key = (section, match.group(1)) if match else None
            if key in values:
                line = f"{key[1]}={values[key]}\n"
                seen.add(key)
        rendered.append(line)
    missing = set(values) - seen
    if missing:
        parser.error(f"upstream config missing keys: {sorted(missing)}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        output.writelines(rendered)
    print(f"Rendered {args.output} with LAN listener {args.listen_ip}; API secret kept in the file")


if __name__ == "__main__":
    main()
