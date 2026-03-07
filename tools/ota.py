#!/usr/bin/env python3
#
# ota.py - cross-platform firmware OTA uploader for ESP32
# Usage:
#   python tools/ota.py
#   python tools/ota.py -d 192.168.4.1
#   python tools/ota.py -d 192.168.4.1 path/to/firmware.bin

from __future__ import annotations

import argparse
import http.client
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from getpass import getpass
from pathlib import Path


DEFAULT_IPS = ("coinbox.local", "4.3.2.1")
CONNECT_TIMEOUT_S = 3.0
UPLOAD_BAR_WIDTH = 24
AUTH_RETRY_LIMIT = 3
CHUNK_SIZE = 64 * 1024
OTA_AUTH_PASSWORD_HEADER = "X-Coinbox-OTA-Password"
OTA_AUTH_RECOVERY_CODE_HEADER = "X-Coinbox-OTA-Recovery-Code"


class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class OtaError(Exception):
    pass


class OtaAuthError(OtaError):
    pass


@dataclass
class AuthHeader:
    name: str
    value: str


class ProgressBar:
    def __init__(self, total_size: int, width: int) -> None:
        self.total_size = total_size
        self.width = width
        self.stream = sys.stderr
        self.enabled = self.stream.isatty()
        self.started = False

    def update(self, sent_bytes: int) -> None:
        if not self.enabled:
            return

        ratio = 1.0 if self.total_size <= 0 else min(1.0, sent_bytes / self.total_size)
        filled = int(ratio * self.width)
        bar = "#" * filled + "-" * (self.width - filled)
        percent = int(ratio * 100)
        self.stream.write(f"\r[{bar}] {percent:3d}%")
        self.stream.flush()
        self.started = True

    def finish(self) -> None:
        if self.enabled and self.started:
            self.stream.write("\n")
            self.stream.flush()
            self.started = False


def parse_args(argv: list[str]) -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    default_fw = script_dir.parent / "build" / "coinbox.bin"

    parser = argparse.ArgumentParser(
        description="Cross-platform firmware OTA uploader for Coinbox.",
    )
    parser.add_argument(
        "-d",
        dest="device",
        metavar="HOST",
        help="ESP32 hostname or IP address",
    )
    parser.add_argument(
        "firmware",
        nargs="?",
        default=str(default_fw),
        help=f"Firmware binary path (default: {default_fw})",
    )
    return parser.parse_args(argv)


def build_opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(NoRedirectHandler())


def fetch_text(host: str, path: str, timeout_s: float) -> tuple[int | None, str]:
    opener = build_opener()
    req = urllib.request.Request(f"http://{host}{path}", method="GET")
    try:
        with opener.open(req, timeout=timeout_s) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as err:
        return err.code, err.read().decode("utf-8", "replace")
    except Exception:
        return None, ""


def detect_auth_requirement(host: str) -> str:
    status, body = fetch_text(host, "/security/status", CONNECT_TIMEOUT_S)
    flag = body.strip()
    if status == 200 and flag == "1":
        return "required"
    if status == 200 and flag == "0":
        return "not_required"
    return "unknown"


def prompt_for_auth(target: str) -> AuthHeader:
    while True:
        print(f"Authentication required for {target}. Please choose a method:")
        print("1. Password")
        print("2. Recovery code")
        selection = input("Selection (default: Password): ").strip()
        if selection in ("", "1"):
            header_name = OTA_AUTH_PASSWORD_HEADER
            prompt = "Password: "
            break
        if selection == "2":
            header_name = OTA_AUTH_RECOVERY_CODE_HEADER
            prompt = "Recovery code: "
            break
        print("Invalid selection. Please choose 1 or 2.", file=sys.stderr)

    while True:
        secret = getpass(prompt)
        if not secret:
            print("A value is required.", file=sys.stderr)
            continue
        if header_name == OTA_AUTH_RECOVERY_CODE_HEADER and not (len(secret) == 4 and secret.isdigit()):
            print("Recovery code must be exactly 4 digits.", file=sys.stderr)
            continue
        return AuthHeader(name=header_name, value=secret)


def render_upload_target(host: str, firmware_path: Path) -> None:
    print(f"Uploading {firmware_path} -> http://{host}/update")


def upload_firmware(host: str, firmware_path: Path, auth: AuthHeader | None) -> None:
    file_size = firmware_path.stat().st_size
    progress = ProgressBar(file_size, UPLOAD_BAR_WIDTH)
    response = None
    body = ""
    conn = http.client.HTTPConnection(host, timeout=CONNECT_TIMEOUT_S)

    render_upload_target(host, firmware_path)
    try:
        conn.connect()
        conn.putrequest("POST", "/update")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(file_size))
        conn.putheader("Connection", "close")
        if auth is not None:
            conn.putheader(auth.name, auth.value)
        conn.endheaders()

        sent_bytes = 0
        progress.update(0)
        with firmware_path.open("rb") as src:
            while True:
                chunk = src.read(CHUNK_SIZE)
                if not chunk:
                    break
                conn.send(chunk)
                sent_bytes += len(chunk)
                progress.update(sent_bytes)

        response = conn.getresponse()
        body = response.read().decode("utf-8", "replace")
    except Exception as exc:
        raise OtaError(f"Upload attempt failed for {host}: {exc}") from exc
    finally:
        progress.finish()
        try:
            conn.close()
        except Exception:
            pass

    if response is None:
        raise OtaError(f"Upload attempt failed for {host}: no HTTP response received")

    if 200 <= response.status < 300:
        return
    if response.status == 401:
        if body.strip():
            raise OtaAuthError(f"Authentication failed for {host} (HTTP 401): {body}")
        raise OtaAuthError(f"Authentication failed for {host} (HTTP 401)")
    if body.strip():
        raise OtaError(f"Error from {host} (HTTP {response.status}): {body}")
    raise OtaError(f"Error from {host} (HTTP {response.status})")


def upload_with_retries(host: str, firmware_path: Path, auth: AuthHeader | None) -> tuple[bool, AuthHeader | None]:
    auth_state = detect_auth_requirement(host)
    active_auth = auth
    if auth_state == "required" and active_auth is None:
        active_auth = prompt_for_auth(host)
    elif auth_state == "not_required":
        active_auth = None

    auth_failures = 0
    while True:
        try:
            upload_firmware(host, firmware_path, active_auth)
            print(f"Success")
            return True, active_auth
        except OtaAuthError as exc:
            print(str(exc), file=sys.stderr)
            auth_failures += 1
            if auth_failures >= AUTH_RETRY_LIMIT:
                print(f"Authentication failed too many times for {host}.", file=sys.stderr)
                return False, active_auth
            active_auth = prompt_for_auth(host)
        except OtaError as exc:
            print(str(exc), file=sys.stderr)
            return False, active_auth


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    firmware_path = Path(args.firmware).expanduser().resolve()
    if not firmware_path.is_file():
        print(f"Error: file not found: {firmware_path}", file=sys.stderr)
        return 1

    targets = [args.device] if args.device else list(DEFAULT_IPS)
    auth: AuthHeader | None = None
    for host in targets:
        ok, auth = upload_with_retries(host, firmware_path, auth)
        if ok:
            return 0

    print("OTA upload failed for all attempted IPs.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
