from __future__ import annotations

import time

import pytest

try:
    from tests.integration.integration_helpers import (
        BOOT_TIMEOUT_S,
        _assert_sound_download_status,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _log_contains_any_since,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_until,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        BOOT_TIMEOUT_S,
        _assert_sound_download_status,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _log_contains_any_since,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_until,
        qemu_bootstrap_instance,
    )


def _unique_name(prefix: str) -> str:
    return f"{prefix}-{int(time.time() * 1000)}"


@pytest.fixture
def qemu_mainapp_instance(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    _skip_to_main_app(base_url, log_path)
    return qemu_bootstrap_instance


def _wait_for_bootstrap_root(base_url: str, timeout_s: float) -> bool:
    def in_bootstrap_mode() -> bool:
        try:
            status, headers, body = _http_get(base_url, "/")
        except Exception:
            return False
        return _is_bootstrap_root_page(status, headers, body)

    return _wait_until(in_bootstrap_mode, timeout_s=timeout_s, poll_s=0.2)


# Test: Reboot endpoint restarts device and system comes back.
# 1. Start from main app mode and confirm `/sounds/` is reachable.
# 2. Call `POST /restart` and accept either immediate response or restart-race disconnect.
# 3. Wait until bootstrap root page is served after reboot.
# 4. Skip back to main app and verify `/sounds/` is reachable again.
def test_restart_endpoint_reboots_device(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200

    restart_status = None
    restart_body = ""
    restart_exc = None
    try:
        restart_status, _, restart_body = _http_request(
            base_url=base_url,
            method="POST",
            path="/restart",
            timeout_s=3.0,
            data=b"",
        )
    except Exception as exc:
        restart_exc = repr(exc)

    if restart_exc is None:
        assert restart_status == 200
        assert "Restarting" in restart_body

    rebooted = _wait_for_bootstrap_root(base_url, timeout_s=float(BOOT_TIMEOUT_S + 20.0))
    assert rebooted, (
        "Device did not reboot into bootstrap after /restart.\n"
        f"restart_status={restart_status}\n"
        f"restart_body={restart_body}\n"
        f"restart_exc={restart_exc}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _skip_to_main_app(base_url, log_path)
    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200


# Test: Format endpoint clears storage files.
# 1. Start from main app mode and upload a sound file.
# 2. Verify uploaded file is downloadable.
# 3. Call `POST /format` and verify success response.
# 4. Verify previously uploaded file is no longer downloadable.
def test_format_endpoint_wipes_uploaded_sounds(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    filename = f"{_unique_name('format')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())
    _assert_sound_download_status(base_url, filename, 200)

    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/format",
        timeout_s=4.0,
        data=b"",
    )
    assert status == 200
    assert "Formatted" in body

    _assert_sound_download_status(base_url, filename, 404)


# Test: Logs endpoint can be fetched in main app mode.
# 1. Start from main app mode and confirm `/sounds/` is reachable.
# 2. Call `GET /logs`.
# 3. Assert success with plain-text response.
# 4. Assert returned logs are non-empty.
def test_logs_endpoint_returns_text_logs(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200

    status, headers, body = _http_get(base_url, "/logs")
    assert status == 200
    assert "text/plain" in headers.get("Content-Type", "")
    assert body.strip() != ""


# Test: OTA endpoint is reachable and handles invalid payload safely.
# 1. Start from main app mode and confirm `/sounds/` is reachable.
# 2. Send an intentionally invalid OTA payload to `POST /update?partition=firmware`.
# 3. Verify OTA handler activity appears in logs.
# 4. Confirm the device remains in main app mode and `/sounds/` is still reachable.
def test_ota_endpoint_probe_in_main_app(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    response_status = None
    response_body = ""
    request_error = None
    try:
        response_status, _, response_body = _http_request(
            base_url=base_url,
            method="POST",
            path="/update?partition=firmware",
            timeout_s=3.0,
            data=b"bad",
            headers={"Content-Type": "application/octet-stream"},
        )
    except Exception as exc:
        # Handler may terminate request without a formal HTTP response.
        request_error = repr(exc)

    ota_seen = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Starting OTA OS", "OTA update failed", "received package is not fit len"],
        ),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert ota_seen, (
        "Did not observe OTA handler activity in logs after probe request.\n"
        f"HTTP status: {response_status}\n"
        f"HTTP body: {response_body}\n"
        f"Request error: {request_error}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200
