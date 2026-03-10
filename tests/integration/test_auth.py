from __future__ import annotations

import json
import time

import pytest

try:
    from tests.integration.integration_helpers import (
        _extract_cookie_pair,
        _get_security_config,
        _http_get,
        _http_request,
        _is_login_redirect,
        _restart_into_bootstrap,
        _set_security_password,
        _skip_to_main_app,
        _test_mp3_bytes,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        _extract_cookie_pair,
        _get_security_config,
        _http_get,
        _http_request,
        _is_login_redirect,
        _restart_into_bootstrap,
        _set_security_password,
        _skip_to_main_app,
        _test_mp3_bytes,
        qemu_bootstrap_instance,
    )


AUTH_PASSWORD = "coinbox-auth-password"


def _unique_name(prefix: str) -> str:
    return f"{prefix}-{int(time.time() * 1000)}"


def _http_post_json(base_url: str, path: str, payload: dict, headers: dict[str, str] | None = None):
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)

    return _http_request(
        base_url=base_url,
        method="POST",
        path=path,
        timeout_s=4.0,
        data=json.dumps(payload).encode("utf-8"),
        headers=req_headers,
    )


def _json_object(body: str, context: str) -> dict:
    value = json.loads(body)
    assert isinstance(value, dict), f"Expected JSON object in {context}, got: {type(value).__name__}"
    return value


def _auth_login(base_url: str, password: str, next_path: str = "/sounds/"):
    status, headers, body = _http_post_json(
        base_url=base_url,
        path="/auth/login",
        payload={"password": password, "next": next_path},
    )
    return status, headers, body



@pytest.fixture
def qemu_mainapp_instance(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    _skip_to_main_app(base_url, log_path)
    return qemu_bootstrap_instance


# Test: Auth can be enabled; wrong password is rejected; correct password grants access.
# 1. Start from main app mode and verify auth is initially disabled.
# 2. Enable auth via `POST /security/config`.
# 3. Verify unauthenticated access to `/sounds/` is redirected to `/login`.
# 4. Verify wrong password login is rejected.
# 5. Verify correct password login returns cookie and allows `/sounds/`.
def test_enable_auth_and_login_flow(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    baseline = _get_security_config(base_url)
    assert baseline.get("password_set") is False

    _set_security_password(base_url, AUTH_PASSWORD)

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")

    wrong_status, _, wrong_body = _auth_login(base_url, "definitely-wrong")
    assert wrong_status == 401
    assert "Unauthorized" in wrong_body

    ok_status, ok_headers, ok_body = _auth_login(base_url, AUTH_PASSWORD)
    assert ok_status == 200
    ok_payload = _json_object(ok_body, "/auth/login")
    assert ok_payload.get("ok") is True
    assert ok_payload.get("password_set") is True
    assert ok_payload.get("next") == "/sounds/"

    cookie = _extract_cookie_pair(ok_headers, "coinbox_auth")
    assert cookie, f"Expected auth cookie from /auth/login. headers={dict(ok_headers)}"

    status, _, _ = _http_request(
        base_url=base_url,
        method="GET",
        path="/sounds/",
        timeout_s=3.0,
        headers={"Cookie": cookie},
    )
    assert status == 200


# Test: Auth setting persists across reboot.
# 1. Start from main app mode and enable auth.
# 2. Restart device to bootstrap using authenticated `/restart`.
# 3. Return to main app via `/skip`.
# 4. Verify unauthenticated `/sounds/` is still blocked.
# 5. Verify logging in with the saved password works after reboot.
def test_auth_persists_after_reboot(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    cookie_before_reboot = _set_security_password(base_url, AUTH_PASSWORD)

    _restart_into_bootstrap(base_url, log_path, headers={"Cookie": cookie_before_reboot})
    _skip_to_main_app(base_url, log_path)

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")

    # Old cookie should no longer be sufficient after restart.
    stale_status, stale_headers, _ = _http_request(
        base_url=base_url,
        method="GET",
        path="/sounds/",
        timeout_s=3.0,
        headers={"Cookie": cookie_before_reboot},
    )
    assert _is_login_redirect(stale_status, stale_headers, "/sounds/")

    login_status, login_headers, login_body = _auth_login(base_url, AUTH_PASSWORD)
    assert login_status == 200
    login_payload = _json_object(login_body, "/auth/login after reboot")
    assert login_payload.get("password_set") is True

    cookie_after_reboot = _extract_cookie_pair(login_headers, "coinbox_auth")
    assert cookie_after_reboot

    allowed_status, _, _ = _http_request(
        base_url=base_url,
        method="GET",
        path="/sounds/",
        timeout_s=3.0,
        headers={"Cookie": cookie_after_reboot},
    )
    assert allowed_status == 200


# Test: Enabling auth blocks protected endpoints when no auth cookie is sent.
# 1. Start from main app mode and upload one file while auth is disabled.
# 2. Enable auth.
# 3. Verify UI entry endpoints redirect to login.
# 4. Verify API/system endpoints return `401 Unauthorized`.
# 5. Verify settings and security change attempts are blocked.
def test_auth_blocks_protected_endpoints(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('auth-block')}.mp3"

    up_status, up_headers, _ = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/{filename}",
        timeout_s=8.0,
        data=_test_mp3_bytes(),
        headers={"Content-Type": "audio/mpeg"},
    )
    assert up_status == 303
    assert up_headers.get("Location") == "/sounds/"

    _set_security_password(base_url, AUTH_PASSWORD)

    for path, expected_next in (
        ("/", "/"),
        ("/sounds/", "/sounds/"),
        ("/settings", "/settings"),
    ):
        status, headers, _ = _http_get(base_url, path)
        assert _is_login_redirect(status, headers, expected_next)

    unauthorized_cases = [
        ("POST", f"/sounds/{_unique_name('blocked-upload')}.mp3", None, None),
        ("GET", f"/sounds/{filename}", None, None),
        (
            "POST",
            f"/sounds/file-meta/{filename}",
            json.dumps({"probability": 55, "volume": 66}).encode("utf-8"),
            {"Content-Type": "application/json"},
        ),
        ("GET", "/network/ips", None, None),
        ("GET", "/network/config", None, None),
        (
            "POST",
            "/network/config",
            json.dumps({"ap_ssid": "blocked-change"}).encode("utf-8"),
            {"Content-Type": "application/json"},
        ),
        ("GET", "/security/config", None, None),
        (
            "POST",
            "/security/config",
            json.dumps({"password": "blocked-new-password"}).encode("utf-8"),
            {"Content-Type": "application/json"},
        ),
        ("POST", f"/audio/playback?name={filename}", b"", None),
        ("GET", "/audio/playback", None, None),
        ("GET", "/audio/test", None, None),
        ("POST", "/audio/test?action=stop", b"", None),
        ("GET", "/logs", None, None),
        ("GET", "/gpio/state", None, None),
        ("GET", "/test/gpio/laser", None, None),
        ("POST", "/test/gpio/laser?level=1", b"", None),
        ("GET", "/test/gpio/hall", None, None),
        ("POST", "/test/gpio/hall?level=1", b"", None),
        ("GET", "/test/gpio/amp-mute", None, None),
        ("POST", "/test/gpio/amp-mute?level=1", b"", None),
        ("GET", "/test/gpio/dac-mute", None, None),
        ("POST", "/test/gpio/dac-mute?level=1", b"", None),
        ("POST", "/restart", b"", None),
        ("POST", "/format", b"", None),
    ]

    for method, path, data, headers in unauthorized_cases:
        status, _, body = _http_request(
            base_url=base_url,
            method=method,
            path=path,
            timeout_s=5.0,
            data=data,
            headers=headers,
        )
        assert status == 401, f"Expected 401 for {method} {path}, got {status}. body={body}"
        assert "Unauthorized" in body


# Test: `/settings/reset` requires auth and must not reboot the device when unauthenticated.
# 1. Start from main app mode and enable auth.
# 2. Call `POST /settings/reset` without cookie.
# 3. Assert `401 Unauthorized`.
# 4. Verify the device remains in main app mode and still redirects `/sounds/` to login.
def test_auth_blocks_settings_reset_endpoint(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    _set_security_password(base_url, AUTH_PASSWORD)

    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/settings/reset",
        timeout_s=3.0,
        data=b"",
    )
    assert status == 401, f"Expected 401 for POST /settings/reset, got {status}. body={body}"
    assert "Unauthorized" in body

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")


# Test: `/boot/config` requires auth for both reads and writes.
# 1. Start from main app mode and enable auth.
# 2. Call unauthenticated `GET /boot/config`.
# 3. Call unauthenticated `POST /boot/config`.
# 4. Assert both requests return `401 Unauthorized`.
def test_auth_blocks_boot_config_endpoint(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    _set_security_password(base_url, AUTH_PASSWORD)

    get_status, _, get_body = _http_get(base_url, "/boot/config")
    assert get_status == 401, f"Expected 401 for GET /boot/config, got {get_status}. body={get_body}"
    assert "Unauthorized" in get_body

    post_status, _, post_body = _http_post_json(
        base_url=base_url,
        path="/boot/config",
        payload={"boot_sound_enabled": False},
    )
    assert post_status == 401, f"Expected 401 for POST /boot/config, got {post_status}. body={post_body}"
    assert "Unauthorized" in post_body


# Test: `/audio/config` requires auth for both reads and writes.
# 1. Start from main app mode and enable auth.
# 2. Call unauthenticated `GET /audio/config`.
# 3. Call unauthenticated `POST /audio/config` with playback settings data.
# 4. Assert both requests return `401 Unauthorized`.
def test_auth_blocks_audio_config_endpoint(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    _set_security_password(base_url, AUTH_PASSWORD)

    get_status, _, get_body = _http_get(base_url, "/audio/config")
    assert get_status == 401, f"Expected 401 for GET /audio/config, got {get_status}. body={get_body}"
    assert "Unauthorized" in get_body

    post_status, _, post_body = _http_post_json(
        base_url=base_url,
        path="/audio/config",
        payload={
            "laser_debounce_ms": 25,
            "hall_debounce_ms": 750,
            "laser_trigger_cooldown_ms": 125,
        },
    )
    assert post_status == 401, f"Expected 401 for POST /audio/config, got {post_status}. body={post_body}"
    assert "Unauthorized" in post_body


# Test: `/skip` and `/recovery` cannot bypass auth once login is enabled.
# 1. Start from main app mode and enable auth.
# 2. Access `GET /skip` and `GET /recovery` without cookie.
# 3. Assert neither endpoint redirects directly to `/sounds/`.
# 4. Assert each request is auth-blocked (login redirect or `401`).
def test_auth_blocks_skip_and_recovery_shortcuts(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    _set_security_password(base_url, AUTH_PASSWORD)

    for path in ("/skip", "/recovery"):
        status, headers, body = _http_get(base_url, path)

        assert not (status == 302 and headers.get("Location") == "/sounds/"), (
            f"{path} redirected directly to /sounds/ while auth is enabled."
        )

        blocked_by_login = _is_login_redirect(status, headers, "/sounds/")
        blocked_by_401 = (status == 401 and "Unauthorized" in body)
        assert blocked_by_login or blocked_by_401, (
            f"Expected {path} to be blocked by auth, got status={status}, "
            f"location={headers.get('Location')}, body={body}"
        )


# Test: Too-long passwords are rejected.
# 1. Start from main app mode.
# 2. Attempt to set password longer than 64 characters.
# 3. Assert rejection response.
# 4. Verify auth remains disabled.
def test_reject_too_long_password(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    too_long_password = "p" * 65

    status, _, body = _http_post_json(
        base_url=base_url,
        path="/security/config",
        payload={"password": too_long_password},
    )
    assert status == 400
    assert "password" in body.lower()

    security_cfg = _get_security_config(base_url)
    assert security_cfg.get("password_set") is False


# Test: Authentication can be removed again.
# 1. Start from main app mode and enable auth.
# 2. Confirm unauthenticated `/sounds/` is blocked.
# 3. Remove auth via authenticated `POST /security/config` with `remove=true`.
# 4. Verify `/sounds/` is accessible without cookie.
# 5. Verify security config reports `password_set=false`.
def test_remove_authentication(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    auth_cookie = _set_security_password(base_url, AUTH_PASSWORD)

    blocked_status, blocked_headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(blocked_status, blocked_headers, "/sounds/")

    status, _, body = _http_post_json(
        base_url=base_url,
        path="/security/config",
        payload={"remove": True},
        headers={"Cookie": auth_cookie},
    )
    assert status == 200
    payload = _json_object(body, "POST /security/config remove")
    assert payload.get("password_set") is False

    open_status, _, _ = _http_get(base_url, "/sounds/")
    assert open_status == 200

    security_cfg = _get_security_config(base_url)
    assert security_cfg.get("password_set") is False
