from __future__ import annotations

import time

import pytest

try:
    from tests.integration.integration_helpers import (
        BOOT_TIMEOUT_S,
        CUSTOM_AP_PASSWORD,
        CUSTOM_AP_SSID,
        CUSTOM_STA_PASSWORD,
        CUSTOM_STA_SSID,
        CUSTOM_UI_PASSWORD,
        _ota_auth_headers,
        _assert_sound_download_status,
        _get_network_config,
        _get_security_config,
        _get_security_status_flag,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _is_login_redirect,
        _log_contains_any_since,
        _set_network_config,
        _set_security_password,
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
        CUSTOM_AP_PASSWORD,
        CUSTOM_AP_SSID,
        CUSTOM_STA_PASSWORD,
        CUSTOM_STA_SSID,
        CUSTOM_UI_PASSWORD,
        _ota_auth_headers,
        _assert_sound_download_status,
        _get_network_config,
        _get_security_config,
        _get_security_status_flag,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _is_login_redirect,
        _log_contains_any_since,
        _set_network_config,
        _set_security_password,
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


def _wait_for_reboot_markers(log_path, log_start_pos: int, timeout_s: float) -> bool:
    reboot_markers = [
        "Rebooting...",
        "rst:0x1 (POWERON_RESET)",
        "rst:0x3 (SW_RESET)",
        "rst:0xc (SW_CPU_RESET)",
        "main_task: Calling app_main()",
        "bootstrap: Bootstrap server started",
    ]
    return _wait_until(
        lambda: _log_contains_any_since(log_path, log_start_pos, reboot_markers),
        timeout_s=timeout_s,
        poll_s=0.2,
    )


# Test: Reboot endpoint restarts device and system comes back.
# 1. Start from main app mode and confirm `/sounds/` is reachable.
# 2. Call `POST /restart` and accept either immediate response or restart-race disconnect.
# 3. Wait until bootstrap root page is served after reboot.
# 4. Skip back to main app and verify `/sounds/` is reachable again.
def test_restart_endpoint_reboots_device(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

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

    reboot_seen = _wait_for_reboot_markers(
        log_path,
        log_start_pos,
        timeout_s=float(BOOT_TIMEOUT_S + 10.0),
    )
    rebooted = _wait_for_bootstrap_root(base_url, timeout_s=float(BOOT_TIMEOUT_S + 40.0))
    assert reboot_seen, (
        "Did not observe reboot markers after /restart.\n"
        f"restart_status={restart_status}\n"
        f"restart_body={restart_body}\n"
        f"restart_exc={restart_exc}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
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


# Test: Settings reset in main app restores defaults and reboots device.
# 1. Start from main app mode and capture baseline network/security config.
# 2. Change as many resettable settings as this build supports:
#    - AP and STA config, when available.
#    - UI auth password.
# 3. Call authenticated `POST /settings/reset` and accept either immediate response or reboot-race disconnect.
# 4. Wait until bootstrap root page is served after reboot.
# 5. Skip back to main app and verify network/security config matches baseline defaults and auth is gone.
def test_reset_settings_endpoint_restores_defaults_and_reboots_device(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    baseline_network = _get_network_config(base_url)
    baseline_security = _get_security_config(base_url)
    assert baseline_security.get("password_set") is False, (
        "Expected auth to be disabled at baseline for main-app settings reset test."
    )

    network_payload = {}
    if baseline_network.get("ap_supported"):
        network_payload["ap_ssid"] = CUSTOM_AP_SSID
        network_payload["ap_password"] = CUSTOM_AP_PASSWORD
    if baseline_network.get("sta_supported"):
        network_payload["sta_ssid"] = CUSTOM_STA_SSID
        network_payload["sta_password"] = CUSTOM_STA_PASSWORD

    if network_payload:
        network_custom = _set_network_config(base_url, network_payload)
        if "ap_ssid" in network_payload:
            assert network_custom.get("ap_ssid") == CUSTOM_AP_SSID
            assert network_custom.get("ap_password_set") is True
        if "sta_ssid" in network_payload:
            assert network_custom.get("sta_ssid") == CUSTOM_STA_SSID
            assert network_custom.get("sta_password_set") is True

    auth_cookie = _set_security_password(base_url, CUSTOM_UI_PASSWORD)
    auth_headers = {"Cookie": auth_cookie}

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")

    security_custom = _get_security_config(base_url, headers=auth_headers)
    assert security_custom.get("password_set") is True

    if network_payload:
        network_custom = _get_network_config(base_url, headers=auth_headers)
        if "ap_ssid" in network_payload:
            assert network_custom.get("ap_ssid") == CUSTOM_AP_SSID
        if "sta_ssid" in network_payload:
            assert network_custom.get("sta_ssid") == CUSTOM_STA_SSID

    reset_status = None
    reset_body = ""
    reset_exc = None
    try:
        reset_status, _, reset_body = _http_request(
            base_url=base_url,
            method="POST",
            path="/settings/reset",
            timeout_s=3.0,
            data=b"",
            headers=auth_headers,
        )
    except Exception as exc:
        reset_exc = repr(exc)

    if reset_exc is None:
        assert reset_status == 200, (
            "Expected 200 from /settings/reset before reboot, "
            f"got status={reset_status}, body={reset_body}"
        )

    rebooted = _wait_for_bootstrap_root(base_url, timeout_s=float(BOOT_TIMEOUT_S + 20.0))
    assert rebooted, (
        "Device did not reboot into bootstrap after /settings/reset.\n"
        f"reset_status={reset_status}\n"
        f"reset_body={reset_body}\n"
        f"reset_exc={reset_exc}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _skip_to_main_app(base_url, log_path)

    network_after_reset = _get_network_config(base_url)
    security_after_reset = _get_security_config(base_url)
    assert network_after_reset == baseline_network
    assert security_after_reset == baseline_security

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert status == 200, (
        "Expected /sounds/ to be accessible after /settings/reset removed auth.\n"
        f"status={status}, location={headers.get('Location')}"
    )


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
# 2. Send an intentionally invalid OTA payload to `POST /update`.
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
            path="/update",
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
            ["Starting OTA firmware update", "OTA update failed", "received package is not fit len"],
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


# Test: Main-app security status endpoint reports auth disabled without requiring auth.
# 1. Start from main app mode with default security config.
# 2. Call `GET /security/status` without auth headers.
# 3. Assert the endpoint returns plain-text `0`.
def test_security_status_reports_disabled_in_main_app(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    flag = _get_security_status_flag(base_url)
    assert flag == "0", f"Expected /security/status to report disabled auth in main app, got: {flag!r}"


# Test: Main-app security status endpoint reports auth enabled without requiring auth.
# 1. Start from main app mode and enable UI auth.
# 2. Call `GET /security/status` without auth headers.
# 3. Assert the endpoint returns plain-text `1`.
def test_security_status_reports_enabled_in_main_app(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    _set_security_password(base_url, CUSTOM_UI_PASSWORD)

    flag = _get_security_status_flag(base_url)
    assert flag == "1", f"Expected /security/status to report enabled auth in main app, got: {flag!r}"


# Test: OTA endpoint requires auth in main app mode once UI auth is enabled.
# 1. Start from main app mode and enable UI auth.
# 2. Call `POST /update` without OTA auth headers.
# 3. Assert `401 Unauthorized`.
# 4. Retry with OTA password header and confirm request reaches the OTA handler.
def test_auth_blocks_ota_endpoint_in_main_app(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    _set_security_password(base_url, CUSTOM_UI_PASSWORD)

    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/update",
        timeout_s=3.0,
        data=b"",
    )
    assert status == 401, f"Expected 401 for unauthenticated POST /update, got {status}. body={body}"
    assert "Unauthorized" in body

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    response_status = None
    response_body = ""
    request_error = None
    try:
        response_status, _, response_body = _http_request(
            base_url=base_url,
            method="POST",
            path="/update",
            timeout_s=3.0,
            data=b"bad",
            headers={
                "Content-Type": "application/octet-stream",
                **_ota_auth_headers(password=CUSTOM_UI_PASSWORD),
            },
        )
    except Exception as exc:
        request_error = repr(exc)

    ota_seen = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Starting OTA firmware update", "OTA update failed", "received package is not fit len"],
        ),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert ota_seen, (
        "Did not observe OTA handler activity after authenticated probe request.\n"
        f"HTTP status: {response_status}\n"
        f"HTTP body: {response_body}\n"
        f"Request error: {request_error}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
