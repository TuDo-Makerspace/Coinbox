from __future__ import annotations

import contextlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError:
        return default


REPO_ROOT = Path(__file__).resolve().parents[2]
TEST_PORT = _env_int("COINBOX_TEST_PORT", 18080)
TEST_BUILD_DIR = os.environ.get("COINBOX_TEST_BUILD_DIR", "build_qemu_integration")
QEMU_LOG_PATH_ENV = os.environ.get("COINBOX_TEST_QEMU_LOG_PATH", "").strip()
BOOT_TIMEOUT_S = float(_env_int("COINBOX_TEST_BOOT_TIMEOUT_S", 30))
QEMU_BUILD_TIMEOUT_S = float(_env_int("COINBOX_TEST_BUILD_TIMEOUT_S", 900))
MAIN_READY_TIMEOUT_S = 20.0
BOOTSTRAP_HTML_MARKER = "Coinbox is starting"
RECOVERY_AP_ATTEMPT_LOG_MARKER = "Creating AP: coinboxrecovery"
RECOVERY_MODE_ENGAGED_LOG_MARKER = "Recovery endpoint hit; countdown aborted"
RECOVERY_SEED_DEFAULT = "test"
QEMU_EFUSE_FACTORY_MAC_OFFSET = 4
QEMU_EFUSE_FACTORY_MAC_LEN = 6
CUSTOM_AP_SSID = "coinbox-reset-ap"
CUSTOM_AP_PASSWORD = "coinbox-reset-pass"
CUSTOM_STA_SSID = "coinbox-reset-sta"
CUSTOM_STA_PASSWORD = "coinbox-reset-pass"
CUSTOM_UI_PASSWORD = "coinbox-reset-password"
OTA_AUTH_PASSWORD_HEADER = "X-Coinbox-OTA-Password"
OTA_AUTH_RECOVERY_CODE_HEADER = "X-Coinbox-OTA-Recovery-Code"
RACE_SKIP_COUNTDOWN_THRESHOLD_S = 1
RACE_RECOVERY_COUNTDOWN_THRESHOLD_S = 1
TEST_MP3_PATH = REPO_ROOT / "tests" / "integration" / "assets" / "test6165ms.mp3"
IPV4_RE = re.compile(
    r"^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\."
    r"(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\."
    r"(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\."
    r"(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)$"
)
_TEST_MP3_BYTES: bytes | None = None
_QEMU_BUILD_DONE = False
_QEMU_BUILD_LOCK = threading.Lock()


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _http_get(base_url: str, path: str, timeout_s: float = 2.0):
    return _http_request(base_url, "GET", path, timeout_s=timeout_s)


def _http_get_bytes(base_url: str, path: str, timeout_s: float = 2.0):
    return _http_request_bytes(base_url, "GET", path, timeout_s=timeout_s)


def _http_request(
    base_url: str,
    method: str,
    path: str,
    timeout_s: float = 2.0,
    data: bytes | None = None,
    headers: dict[str, str] | None = None,
):
    opener = urllib.request.build_opener(_NoRedirectHandler())
    req = urllib.request.Request(f"{base_url}{path}", data=data, method=method)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    try:
        with opener.open(req, timeout=timeout_s) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return resp.status, resp.headers, body
    except urllib.error.HTTPError as err:
        body = err.read().decode("utf-8", errors="replace")
        return err.code, err.headers, body


def _http_request_bytes(
    base_url: str,
    method: str,
    path: str,
    timeout_s: float = 2.0,
    data: bytes | None = None,
    headers: dict[str, str] | None = None,
):
    opener = urllib.request.build_opener(_NoRedirectHandler())
    req = urllib.request.Request(f"{base_url}{path}", data=data, method=method)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    try:
        with opener.open(req, timeout=timeout_s) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as err:
        return err.code, err.headers, err.read()


def _http_post_json(
    base_url: str,
    path: str,
    payload: dict,
    timeout_s: float = 2.0,
    headers: dict[str, str] | None = None,
):
    data = json.dumps(payload).encode("utf-8")
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    return _http_request(
        base_url=base_url,
        method="POST",
        path=path,
        timeout_s=timeout_s,
        data=data,
        headers=req_headers,
    )


def _test_mp3_bytes() -> bytes:
    global _TEST_MP3_BYTES
    if _TEST_MP3_BYTES is None:
        if not TEST_MP3_PATH.is_file():
            pytest.fail(f"Missing integration test MP3 fixture: {TEST_MP3_PATH}")
        _TEST_MP3_BYTES = TEST_MP3_PATH.read_bytes()
        if not _TEST_MP3_BYTES:
            pytest.fail(f"Integration test MP3 fixture is empty: {TEST_MP3_PATH}")
    return _TEST_MP3_BYTES


def _wait_until(predicate, timeout_s: float, poll_s: float = 0.2) -> bool:
    deadline = time.time() + timeout_s

    while time.time() < deadline:
        if predicate():
            return True

        time.sleep(poll_s)
    return False


def _is_bootstrap_root_page(status: int, headers, body: str) -> bool:
    content_type = headers.get("Content-Type", "")
    if status != 200:
        return False
    if "text/html" not in content_type:
        return False
    if BOOTSTRAP_HTML_MARKER not in body:
        return False
    return 'href="/skip"' in body


def _extract_bootstrap_countdown_seconds(body: str) -> int | None:
    # Primary source: rendered badge value.
    badge_match = re.search(r'id="countdown"\s*>\s*(\d+)\s*<', body)
    if badge_match:
        return int(badge_match.group(1))

    # Fallback source: rendered JS literal.
    script_match = re.search(r"let\s+remaining\s*=\s*(\d+)\s*;", body)
    if script_match:
        return int(script_match.group(1))

    return None


def _is_main_app_ready(base_url: str) -> bool:
    try:
        status_code, headers, _ = _http_get(base_url, "/sounds/")
    except Exception:
        return False
    if status_code == 200:
        return True
    if status_code == 302 and headers.get("Location", "").startswith("/login"):
        return True
    return False


def _is_upload_handler_ready(base_url: str) -> bool:
    # Probe POST route without creating files: .wav is always rejected by upload validation.
    try:
        status, _, body = _http_request(
            base_url=base_url,
            method="POST",
            path="/sounds/__startup_probe__.wav",
            timeout_s=2.0,
            data=b"x",
            headers={"Content-Type": "audio/wav"},
        )
    except Exception:
        return False

    if status == 400 and "Only audio files are allowed" in body:
        return True
    if status == 401:
        return True
    return False


def _is_recovery_bootstrap_page(status: int, headers, body: str) -> bool:
    if not _is_bootstrap_root_page(status, headers, body):
        return False
    if not re.search(r"let\s+isRecovery\s*=\s*1\s*;", body):
        return False
    if not re.search(r"let\s+autoRefresh\s*=\s*0\s*;", body):
        return False
    return True


def _is_login_redirect(status_code: int, headers, expected_next: str) -> bool:
    if status_code != 302:
        return False
    location = headers.get("Location", "")
    if not location.startswith("/login"):
        return False
    return f"next={expected_next}" in location


def _ensure_auth_enabled(base_url: str):
    # Main app can briefly serve some routes before all URI handlers are registered.
    # Retry auth enabling for a short window to avoid startup race flakes.
    deadline = time.time() + 8.0
    last_status = None
    last_body = ""

    while time.time() < deadline:
        # If auth is already enabled, /sounds/ should redirect to login.
        status, headers, _ = _http_get(base_url, "/sounds/")
        if _is_login_redirect(status, headers, "/sounds/"):
            return

        # Enable auth via security config while auth is currently open.
        status, _, body = _http_post_json(base_url, "/security/config", {"password": "coinbox-test-password"})
        last_status = status
        last_body = body

        if status == 200:
            assert '"password_set":true' in body, f"Unexpected security config response: {body}"
            return

        # Transient states during handoff/handler registration.
        if status in (307, 401, 404, 503):
            time.sleep(0.2)
            continue

        assert False, f"Failed to enable auth: status={status}, body={body}"

    pytest.fail(
        "Timed out enabling auth.\n"
        f"Last status: {last_status}\n"
        f"Last body: {last_body}"
    )


def _enter_recovery_mode(base_url: str, log_path: Path | None = None):
    log_start_pos = 0
    if log_path is not None and log_path.exists():
        log_start_pos = log_path.stat().st_size

    status, _, body = _http_get(base_url, "/recovery")
    assert status == 200, f"Expected 200 from /recovery, got {status}"
    assert "Recovery mode engaged" in body

    if log_path is None:
        return

    saw_recovery_marker = _wait_until(
        lambda: _log_contains_any_since(log_path, log_start_pos, [RECOVERY_MODE_ENGAGED_LOG_MARKER]),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert saw_recovery_marker, (
        "Did not observe recovery endpoint log after /recovery.\n"
        f"Expected marker: {RECOVERY_MODE_ENGAGED_LOG_MARKER}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


def _json_load_object(body: str, context: str) -> dict:
    try:
        value = json.loads(body)
    except json.JSONDecodeError as exc:
        pytest.fail(f"Failed to decode JSON from {context}: {exc}\nBody:\n{body}")

    assert isinstance(value, dict), f"Expected JSON object from {context}, got: {type(value).__name__}"
    return value


def _extract_cookie_pair(headers, cookie_name: str) -> str | None:
    set_cookie = headers.get("Set-Cookie", "")
    if not set_cookie:
        return None

    first_pair = set_cookie.split(";", 1)[0].strip()
    if first_pair.startswith(f"{cookie_name}="):
        return first_pair
    return None


def _strip_optional_quotes(value: str) -> str:
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def _load_recovery_seed() -> str:
    env_path = REPO_ROOT / ".env"
    if not env_path.is_file():
        return RECOVERY_SEED_DEFAULT

    for raw_line in env_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip() == "COINBOX_RECOVERY_SEED":
            return _strip_optional_quotes(value.strip())

    return RECOVERY_SEED_DEFAULT


def _get_qemu_factory_mac() -> str:
    efuse_path = REPO_ROOT / TEST_BUILD_DIR / "qemu_efuse.bin"
    assert efuse_path.is_file(), f"Missing QEMU efuse image: {efuse_path}"

    efuse_bytes = efuse_path.read_bytes()
    start = QEMU_EFUSE_FACTORY_MAC_OFFSET
    end = start + QEMU_EFUSE_FACTORY_MAC_LEN
    assert len(efuse_bytes) >= end, f"QEMU efuse image too short: {efuse_path}"

    mac = efuse_bytes[start:end]
    return ":".join(f"{byte:02x}" for byte in mac)


def _expected_recovery_code() -> int:
    script_path = REPO_ROOT / "tools" / "recovery_code.py"
    assert script_path.is_file(), f"Missing recovery code helper script: {script_path}"

    output = subprocess.check_output(
        ["python3", str(script_path), _load_recovery_seed(), _get_qemu_factory_mac()],
        cwd=REPO_ROOT,
        text=True,
    ).strip()
    assert output.isdigit(), f"Unexpected recovery code output: {output!r}"
    return int(output)


def _authenticate_recovery(
    base_url: str,
    *,
    password: str | None = None,
    recovery_code: int | None = None,
) -> dict[str, str]:
    assert (password is None) != (recovery_code is None), (
        "Provide exactly one of password or recovery_code when authenticating bootstrap recovery."
    )

    payload: dict[str, object] = {}
    if password is not None:
        payload["password"] = password
    else:
        payload["recovery_code"] = recovery_code

    status, headers, body = _http_post_json(base_url, "/recovery/auth", payload)
    assert status == 200, (
        f"Bootstrap recovery auth failed. status={status}, body={body}, headers={dict(headers)}"
    )

    cookie = _extract_cookie_pair(headers, "coinbox_recovery_auth")
    if cookie:
        if body.strip().startswith("{"):
            payload_obj = _json_load_object(body, "POST /recovery/auth")
            assert payload_obj.get("ok") is True, f"Unexpected bootstrap auth JSON body: {payload_obj}"
        return {"Cookie": cookie}

    payload_obj = _json_load_object(body, "POST /recovery/auth")
    assert payload_obj.get("ok") is True, f"Unexpected bootstrap auth response: {payload_obj}"
    token = payload_obj.get("token")
    assert isinstance(token, str) and token, f"Missing bootstrap auth token in response: {payload_obj}"
    return {"X-Recovery-Auth": token}


def _ota_auth_headers(
    *,
    password: str | None = None,
    recovery_code: int | None = None,
) -> dict[str, str]:
    assert (password is None) != (recovery_code is None), (
        "Provide exactly one of password or recovery_code when building OTA auth headers."
    )

    if password is not None:
        return {OTA_AUTH_PASSWORD_HEADER: password}
    return {OTA_AUTH_RECOVERY_CODE_HEADER: f"{int(recovery_code):04d}"}


def _wait_for_json_200(
    base_url: str,
    path: str,
    headers: dict[str, str] | None = None,
    timeout_s: float = 8.0,
) -> dict:
    deadline = time.time() + timeout_s
    last_status = None
    last_body = ""

    while time.time() < deadline:
        status, _, body = _http_request(
            base_url=base_url,
            method="GET",
            path=path,
            timeout_s=2.0,
            headers=headers,
        )
        last_status = status
        last_body = body

        if status == 200:
            return _json_load_object(body, path)

        if status in (307, 401, 404, 503):
            time.sleep(0.2)
            continue

        pytest.fail(f"Unexpected status from {path}: {status}\nBody:\n{body}")

    pytest.fail(
        f"Timed out waiting for 200 from {path}.\n"
        f"Last status: {last_status}\n"
        f"Last body: {last_body}"
    )


def _get_network_config(base_url: str, headers: dict[str, str] | None = None) -> dict:
    return _wait_for_json_200(base_url, "/network/config", headers=headers)


def _get_security_config(base_url: str, headers: dict[str, str] | None = None) -> dict:
    return _wait_for_json_200(base_url, "/security/config", headers=headers)


def _get_security_status_flag(
    base_url: str,
    headers: dict[str, str] | None = None,
    timeout_s: float = 8.0,
) -> str:
    deadline = time.time() + timeout_s
    last_status = None
    last_body = ""

    while time.time() < deadline:
        status, _, body = _http_request(
            base_url=base_url,
            method="GET",
            path="/security/status",
            timeout_s=2.0,
            headers=headers,
        )
        last_status = status
        last_body = body

        if status == 200:
            flag = body.strip()
            assert flag in ("0", "1"), (
                "Expected /security/status to return plain-text '0' or '1'.\n"
                f"Got: {body!r}"
            )
            return flag

        if status in (307, 401, 404, 503):
            time.sleep(0.2)
            continue

        pytest.fail(f"Unexpected status from /security/status: {status}\nBody:\n{body}")

    pytest.fail(
        "Timed out waiting for 200 from /security/status.\n"
        f"Last status: {last_status}\n"
        f"Last body: {last_body}"
    )


def _set_network_config(
    base_url: str,
    payload: dict,
    headers: dict[str, str] | None = None,
) -> dict:
    status, _, body = _http_post_json(
        base_url=base_url,
        path="/network/config",
        payload=payload,
        headers=headers,
    )
    assert status == 200, f"Failed to set /network/config. status={status}, body={body}"
    return _json_load_object(body, "POST /network/config")


def _set_security_password(
    base_url: str,
    password: str,
    headers: dict[str, str] | None = None,
) -> str:
    status, resp_headers, body = _http_post_json(
        base_url=base_url,
        path="/security/config",
        payload={"password": password},
        headers=headers,
    )
    assert status == 200, f"Failed to set /security/config password. status={status}, body={body}"

    data = _json_load_object(body, "POST /security/config")
    assert data.get("password_set") is True, f"Expected password_set=true after auth enable, got: {data}"

    cookie_pair = _extract_cookie_pair(resp_headers, "coinbox_auth")
    assert cookie_pair, f"Expected Set-Cookie for auth, got headers: {dict(resp_headers)}"
    return cookie_pair


def _skip_to_main_app(base_url: str, log_path: Path):
    status, headers, body = _http_get(base_url, "/skip")
    assert status == 200, f"/skip failed in bootstrap/recovery. status={status}, body={body}"
    assert headers.get("Cache-Control") == "no-store"
    assert "Starting main application" in body

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after /skip.\nLog tail:\n{_tail_log(log_path)}"

    upload_ready = _wait_until(
        lambda: _is_upload_handler_ready(base_url),
        timeout_s=8.0,
        poll_s=0.2,
    )
    assert upload_ready, f"Upload handler did not become ready after /skip.\nLog tail:\n{_tail_log(log_path)}"


def _restart_into_bootstrap(
    base_url: str,
    log_path: Path,
    headers: dict[str, str] | None = None,
):
    # Restart can race with connection close; do not require a specific HTTP result.
    try:
        _http_request(
            base_url=base_url,
            method="POST",
            path="/restart",
            timeout_s=2.0,
            data=b"",
            headers=headers,
        )
    except Exception:
        pass

    def bootstrap_ready() -> bool:
        try:
            status, resp_headers, body = _http_get(base_url, "/")
        except Exception:
            return False
        return _is_bootstrap_root_page(status, resp_headers, body)

    ready = _wait_until(
        bootstrap_ready,
        timeout_s=float(BOOT_TIMEOUT_S + 15.0),
        poll_s=0.2,
    )
    assert ready, f"Device did not reboot back to bootstrap mode.\nLog tail:\n{_tail_log(log_path)}"


def _reset_settings_from_recovery(base_url: str, headers: dict[str, str] | None = None):
    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/settings/reset",
        timeout_s=2.0,
        data=b"",
        headers=headers,
    )
    assert status == 200, f"/settings/reset failed in recovery. status={status}, body={body}"
    assert "Settings reset to defaults" in body


def _format_storage_from_recovery(base_url: str, headers: dict[str, str] | None = None):
    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/format",
        timeout_s=2.0,
        data=b"",
        headers=headers,
    )
    assert status == 200, f"/format failed in recovery. status={status}, body={body}"
    assert "Storage formatted" in body


def _upload_sound_file(
    base_url: str,
    filename: str,
    payload: bytes,
    headers: dict[str, str] | None = None,
):
    req_headers = {"Content-Type": "audio/mpeg"}
    if headers:
        req_headers.update(headers)

    status, resp_headers, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/{filename}",
        timeout_s=10.0,
        data=payload,
        headers=req_headers,
    )
    assert status == 303, f"Upload failed for {filename}. status={status}, body={body}"
    assert resp_headers.get("Location") == "/sounds/"


def _assert_sound_download_status(
    base_url: str,
    filename: str,
    expected_status: int,
    timeout_s: float = 8.0,
):
    status, _, body = _http_get(base_url, f"/sounds/{filename}", timeout_s=timeout_s)
    assert status == expected_status, (
        f"Unexpected status for /sounds/{filename}. "
        f"Expected {expected_status}, got {status}. Body:\n{body}"
    )


def _assert_network_ips_payload(base_url: str):
    status, headers, body = _http_get(base_url, "/network/ips")
    assert status == 200, f"/network/ips did not return 200 in recovery. status={status}, body={body}"
    assert "application/json" in headers.get("Content-Type", "")

    data = json.loads(body)
    assert isinstance(data, dict)
    for key in ("ap", "sta", "sta_ssid"):
        assert key in data, f"Missing key in /network/ips payload: {key}"
        assert isinstance(data[key], str), f"Expected string value for key '{key}'"

    for key in ("ap", "sta"):
        if data[key]:
            assert IPV4_RE.match(data[key]), f"Invalid IPv4 string for key '{key}': {data[key]}"

    assert len(data["sta_ssid"]) <= 32, f"Unexpected SSID length: {len(data['sta_ssid'])}"
    assert data["ap"] or data["sta"] or data["sta_ssid"], "Expected at least one non-empty IP/SSID field"


def _assert_recovery_persists(base_url: str, duration_s: float, poll_s: float = 0.5):
    deadline = time.time() + duration_s
    while time.time() < deadline:
        status, headers, body = _http_get(base_url, "/")
        assert _is_recovery_bootstrap_page(status, headers, body), (
            "Expected to stay in recovery bootstrap page, but page changed.\n"
            f"status={status}\n"
            f"body snippet:\n{body[:300]}"
        )

        # During bootstrap/recovery, main-app routes should not be active.
        sounds_status, sounds_headers, _ = _http_get(base_url, "/sounds/")
        assert sounds_status == 307 and sounds_headers.get("Location") == "/", (
            "Expected /sounds/ to still be handled by bootstrap while in recovery.\n"
            f"status={sounds_status}, location={sounds_headers.get('Location')}"
        )
        time.sleep(poll_s)


def _log_contains_any_since(path: Path, start_pos: int, markers: list[str]) -> bool:
    if not path.exists():
        return False

    with path.open("r", encoding="utf-8", errors="replace") as f:
        if start_pos > 0:
            f.seek(start_pos)
        text = f.read()
    return any(marker in text for marker in markers)


def _wait_for_bootstrap_countdown_threshold(
    base_url: str,
    threshold_s: int,
    timeout_s: float,
    poll_s: float = 0.1,
) -> int | None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            status, headers, body = _http_get(base_url, "/")
        except Exception:
            time.sleep(poll_s)
            continue

        if not _is_bootstrap_root_page(status, headers, body):
            return None

        countdown_s = _extract_bootstrap_countdown_seconds(body)
        if countdown_s is not None and countdown_s <= threshold_s:
            return countdown_s

        time.sleep(poll_s)
    return None


def _tail_log(path: Path, max_lines: int = 120) -> str:
    if not path.exists():
        return ""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(lines[-max_lines:])


def _capture_qemu_output(stream, log_file):
    for line in iter(stream.readline, ""):
        log_file.write(line)
        log_file.flush()

    with contextlib.suppress(Exception):
        stream.close()


def _ensure_prerequisites():
    if shutil.which("idf.py") is None:
        pytest.skip("idf.py not found in PATH. Export ESP-IDF first.")
    if shutil.which("qemu-system-xtensa") is None:
        pytest.skip("qemu-system-xtensa not found in PATH. Install/export ESP-IDF tools first.")


def _idf_qemu_base_cmd() -> list[str]:
    return [
        "idf.py",
        "-B",
        TEST_BUILD_DIR,
        "-D",
        "SDKCONFIG=sdkconfig.qemu",
        "-D",
        "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.qemu",
    ]


def _ensure_qemu_firmware_built():
    global _QEMU_BUILD_DONE
    if _QEMU_BUILD_DONE:
        return

    with _QEMU_BUILD_LOCK:
        if _QEMU_BUILD_DONE:
            return

        build_cmd = _idf_qemu_base_cmd() + ["build"]
        try:
            result = subprocess.run(
                build_cmd,
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
                timeout=QEMU_BUILD_TIMEOUT_S,
            )
        except subprocess.TimeoutExpired:
            pytest.fail(
                "Timed out while building firmware for QEMU integration tests.\n"
                f"Command: {' '.join(build_cmd)}\n"
                f"Timeout: {QEMU_BUILD_TIMEOUT_S}s"
            )

        if result.returncode != 0:
            stdout_tail = "\n".join(result.stdout.splitlines()[-120:])
            stderr_tail = "\n".join(result.stderr.splitlines()[-120:])
            pytest.fail(
                "Failed to build firmware for QEMU integration tests.\n"
                f"Command: {' '.join(build_cmd)}\n"
                f"stdout tail:\n{stdout_tail}\n"
                f"stderr tail:\n{stderr_tail}"
            )

        _QEMU_BUILD_DONE = True


def _ensure_port_free(port: int):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        in_use = sock.connect_ex(("127.0.0.1", port)) == 0
    if in_use:
        pytest.skip(f"TCP port {port} is already in use. Set COINBOX_TEST_PORT to a free port.")


def _is_pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _list_repo_qemu_pids() -> list[int]:
    # Only target qemu-system-xtensa processes launched from this repo.
    result = subprocess.run(
        ["ps", "-eo", "pid=,args="],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []

    repo_marker = str(REPO_ROOT)
    pids: list[int] = []
    for line in result.stdout.splitlines():
        entry = line.strip()
        if not entry:
            continue
        pid_str, _, args = entry.partition(" ")
        if not pid_str.isdigit():
            continue
        pid = int(pid_str)
        if pid == os.getpid():
            continue
        if "qemu-system-xtensa" not in args:
            continue
        if repo_marker not in args:
            continue
        pids.append(pid)
    return pids


def _kill_stale_qemu_instances() -> int:
    pids = _list_repo_qemu_pids()
    if not pids:
        return 0

    for pid in pids:
        with contextlib.suppress(ProcessLookupError):
            os.kill(pid, signal.SIGTERM)

    term_deadline = time.time() + 8.0
    while time.time() < term_deadline:
        alive = [pid for pid in pids if _is_pid_alive(pid)]
        if not alive:
            return len(pids)
        time.sleep(0.1)

    for pid in pids:
        if _is_pid_alive(pid):
            with contextlib.suppress(ProcessLookupError):
                os.kill(pid, signal.SIGKILL)

    return len(pids)


def _stop_process_group(proc: subprocess.Popen):
    if proc.poll() is not None:
        return

    with contextlib.suppress(ProcessLookupError):
        os.killpg(proc.pid, signal.SIGTERM)

    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(proc.pid, signal.SIGKILL)
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=5)


@pytest.fixture
def qemu_bootstrap_instance(tmp_path: Path):
    _ensure_prerequisites()
    _ensure_qemu_firmware_built()

    base_url = f"http://127.0.0.1:{TEST_PORT}"
    if QEMU_LOG_PATH_ENV:
        log_path = Path(QEMU_LOG_PATH_ENV).expanduser()
        if not log_path.is_absolute():
            log_path = REPO_ROOT / log_path
        log_path.parent.mkdir(parents=True, exist_ok=True)
    else:
        log_path = tmp_path / "qemu_skip_test.log"

    def in_bootstrap_mode() -> bool:
        try:
            status, headers, body = _http_get(base_url, "/")
        except Exception:
            return False
        return _is_bootstrap_root_page(status, headers, body)

    _kill_stale_qemu_instances()
    _ensure_port_free(TEST_PORT)

    qemu_args = f"-nic user,model=open_eth,hostfwd=tcp::{TEST_PORT}-:80"
    cmd = _idf_qemu_base_cmd() + ["qemu", "--qemu-extra-args", qemu_args]

    log_file = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        cmd,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,
        preexec_fn=os.setsid,
    )

    if proc.stdout is None:
        log_file.close()
        pytest.fail("Failed to capture QEMU output stream.")

    output_thread = threading.Thread(
        target=_capture_qemu_output,
        args=(proc.stdout, log_file),
        daemon=True,
    )
    output_thread.start()

    try:
        boot_ok = _wait_until(
            in_bootstrap_mode,
            timeout_s=BOOT_TIMEOUT_S,
        )
        assert boot_ok, (
            "QEMU did not reach bootstrap mode in time.\n"
            f"Command: {' '.join(cmd)}\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        yield {"base_url": base_url, "log_path": log_path, "process": proc}
    finally:
        _stop_process_group(proc)
        output_thread.join(timeout=2.0)
        log_file.close()
