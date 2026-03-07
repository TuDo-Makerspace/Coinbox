#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import hmac
import re
import sys


MAC_HEX_RE = re.compile(r"^[0-9a-fA-F]{12}$")
BLACKLISTED_CODES = {
    "1234",
    "4321",
}


def parse_mac(value: str) -> bytes:
    normalized = value.strip().replace(":", "").replace("-", "").replace(".", "")
    if not MAC_HEX_RE.fullmatch(normalized):
        raise argparse.ArgumentTypeError(
            "MAC must be 12 hex digits, optionally separated by ':' or '-'."
        )
    return bytes.fromhex(normalized)


def parse_seed(value: str, *, hex_mode: bool) -> bytes:
    if hex_mode:
        normalized = value.strip().replace(" ", "")
        if len(normalized) % 2 != 0:
            raise argparse.ArgumentTypeError("Hex seed must contain an even number of digits.")
        try:
            return bytes.fromhex(normalized)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"Invalid hex seed: {exc}") from exc
    return value.encode("utf-8")


def is_blacklisted_code(code: str) -> bool:
    if code in BLACKLISTED_CODES:
        return True
    return len(code) == 4 and code.count(code[0]) == 4


def _candidate_digest(seed: bytes, mac: bytes, attempt: int) -> bytes:
    if attempt == 0:
        message = mac
    else:
        message = mac + b"#" + attempt.to_bytes(4, "big")
    return hmac.new(seed, message, hashlib.sha256).digest()


def recovery_code(seed: bytes, mac: bytes) -> str:
    for attempt in range(256):
        digest = _candidate_digest(seed, mac, attempt)
        value = int.from_bytes(digest[:4], "big")
        code = f"{value % 10000:04d}"
        if not is_blacklisted_code(code):
            return code

    raise RuntimeError("Could not generate a non-blacklisted recovery code.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic 4-digit recovery code from a seed and MAC address."
    )
    parser.add_argument("seed", help="Seed text, or hex bytes when --seed-hex is used.")
    parser.add_argument("mac", help="MAC address, e.g. AA:BB:CC:DD:EE:FF")
    parser.add_argument(
        "--seed-hex",
        action="store_true",
        help="Interpret the seed argument as hex bytes instead of UTF-8 text.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print normalized inputs together with the generated code.",
    )
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        seed = parse_seed(args.seed, hex_mode=args.seed_hex)
        mac = parse_mac(args.mac)
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))

    code = recovery_code(seed, mac)
    if args.verbose:
        print(f"seed={seed.hex()}")
        print(f"mac={mac.hex().upper()}")
        print(f"code={code}")
    else:
        print(code)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
