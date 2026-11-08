#!/usr/bin/env python3
"""Generate line 3 (HMAC) for Pastebin license. Match license_native.cpp kPepper / kKeyXor."""
import hmac
import hashlib
import sys

PEPPER = "xorel.pepper.v2.8f3c"
PASTE_ID = "5ZWQL5Zi"
KEY = bytes.fromhex(
    "ac40ed0fa1a86c1a3eafb4ae5f909684e635ce444c7df3c3339dada63131deee"
)


def sign(password: str, date_ymd: str) -> str:
    msg = f"{PEPPER}|{password}|{date_ymd}|{PASTE_ID}".encode()
    return hmac.new(KEY, msg, hashlib.sha256).hexdigest()


def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: sign_license.py PASSWORD YYYY/MM/DD", file=sys.stderr)
        sys.exit(1)
    pwd = sys.argv[1]
    raw = sys.argv[2].replace("/", "-")
    y, m, d = raw.split("-")
    date_key = f"{int(y):04d}-{int(m):02d}-{int(d):02d}"
    sig = sign(pwd, date_key)
    print("Pastebin body (3 lines):")
    print(pwd)
    print(f"{int(y):04d}/{int(m):02d}/{int(d):02d}")
    print(sig)


if __name__ == "__main__":
    main()
