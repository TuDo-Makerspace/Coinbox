from __future__ import annotations

import json
import re
import time

try:
    from tests.integration.integration_helpers import (
        BOOT_TIMEOUT_S,
        CUSTOM_AP_PASSWORD,
        CUSTOM_AP_SSID,
        RECOVERY_AP_ATTEMPT_LOG_MARKER,
        CUSTOM_STA_PASSWORD,
        CUSTOM_STA_SSID,
        CUSTOM_UI_PASSWORD,
        MAIN_READY_TIMEOUT_S,
        RACE_RECOVERY_COUNTDOWN_THRESHOLD_S,
        RACE_SKIP_COUNTDOWN_THRESHOLD_S,
        _assert_network_ips_payload,
        _assert_recovery_persists,
        _assert_sound_download_status,
        _ensure_auth_enabled,
        _enter_recovery_mode,
        _extract_bootstrap_countdown_seconds,
        _format_storage_from_recovery,
        _get_network_config,
        _get_security_config,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _is_login_redirect,
        _is_main_app_ready,
        _is_recovery_bootstrap_page,
        _log_contains_any_since,
        _reset_settings_from_recovery,
        _restart_into_bootstrap,
        _set_network_config,
        _set_security_password,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_for_bootstrap_countdown_threshold,
        _wait_until,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        BOOT_TIMEOUT_S,
        CUSTOM_AP_PASSWORD,
        CUSTOM_AP_SSID,
        RECOVERY_AP_ATTEMPT_LOG_MARKER,
        CUSTOM_STA_PASSWORD,
        CUSTOM_STA_SSID,
        CUSTOM_UI_PASSWORD,
        MAIN_READY_TIMEOUT_S,
        RACE_RECOVERY_COUNTDOWN_THRESHOLD_S,
        RACE_SKIP_COUNTDOWN_THRESHOLD_S,
        _assert_network_ips_payload,
        _assert_recovery_persists,
        _assert_sound_download_status,
        _ensure_auth_enabled,
        _enter_recovery_mode,
        _extract_bootstrap_countdown_seconds,
        _format_storage_from_recovery,
        _get_network_config,
        _get_security_config,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _is_login_redirect,
        _is_main_app_ready,
        _is_recovery_bootstrap_page,
        _log_contains_any_since,
        _reset_settings_from_recovery,
        _restart_into_bootstrap,
        _set_network_config,
        _set_security_password,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_for_bootstrap_countdown_threshold,
        _wait_until,
        qemu_bootstrap_instance,
    )


def _json_object(body: str, context: str) -> dict:
    value = json.loads(body)
    assert isinstance(value, dict), f"Expected JSON object from {context}, got {type(value).__name__}"
    return value


BOOTSTRAP_EXTEND_NOTE_DEFAULT = "Interrupt the laser beam three times to extend the countdown to 60 seconds."
BOOTSTRAP_EXTEND_NOTE_EXTENDED = "Laser beam interrupted three times. Countdown extended."


def _set_test_gpio_level(base_url: str, name: str, level: int):
    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/test/gpio/{name}?level={level}",
        timeout_s=3.0,
        data=b"",
    )
    assert status == 200, (
        f"Failed to set /test/gpio/{name}?level={level}. "
        f"status={status}, body={body}"
    )
    payload = _json_object(body, f"POST /test/gpio/{name}")
    assert payload.get("level") == level


def _get_test_gpio_level(base_url: str, name: str) -> int:
    status, _, body = _http_get(base_url, f"/test/gpio/{name}")
    assert status == 200, f"Failed to get /test/gpio/{name}. status={status}, body={body}"
    payload = _json_object(body, f"GET /test/gpio/{name}")
    level = payload.get("level")
    assert level in (0, 1), f"Unexpected level for /test/gpio/{name}: {level}"
    return level


def _inject_laser_beam_break(base_url: str):
    # One beam break is defined as clear -> blocked.
    _set_test_gpio_level(base_url, "laser", 0)
    _set_test_gpio_level(base_url, "laser", 1)


def _get_bootstrap_countdown_seconds(base_url: str) -> int:
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body), (
        "Expected bootstrap root page while reading countdown.\n"
        f"status={status}\n"
        f"body snippet:\n{body[:300]}"
    )
    countdown = _extract_bootstrap_countdown_seconds(body)
    assert countdown is not None, "Could not parse bootstrap countdown from page."
    return countdown


def _extract_bootstrap_extend_note_text(body: str) -> str | None:
    note_match = re.search(r'id="extend-note"\s*>\s*([^<]+)\s*<', body)
    if note_match:
        return note_match.group(1).strip()
    return None


def _get_bootstrap_extend_note_text(base_url: str) -> str:
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body), (
        "Expected bootstrap root page while reading extension note.\n"
        f"status={status}\n"
        f"body snippet:\n{body[:300]}"
    )
    note = _extract_bootstrap_extend_note_text(body)
    assert note is not None, "Could not parse extension note from bootstrap page."
    return note

# Test: Bootstrap unknown route redirects to root.
# 1. Confirm bootstrap mode is active via unique root-page marker.
# 2. Call an unknown route.
# 3. Assert bootstrap returns `307` with `Location: /`.
def test_bootstrap_404_redirects_to_root(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]

    # Confirm bootstrap page is active first.
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    # Unknown route should hit bootstrap 404 handler and redirect to "/".
    status, headers, _ = _http_get(base_url, "/this-path-should-not-exist")
    assert status == 307
    assert headers.get("Location") == "/"


# Test: Bootstrap startup attempts to switch AP to recovery SSID.
# 1. Confirm bootstrap mode is active.
# 2. Assert startup logs include recovery-AP switch attempt marker.
def test_bootstrap_starts_with_recovery_ap_attempt(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    saw_recovery_ap_attempt = _wait_until(
        lambda: _log_contains_any_since(log_path, 0, [RECOVERY_AP_ATTEMPT_LOG_MARKER]),
        timeout_s=2.0,
        poll_s=0.1,
    )
    assert saw_recovery_ap_attempt, (
        "Did not observe recovery AP startup attempt in logs.\n"
        f"Expected marker: {RECOVERY_AP_ATTEMPT_LOG_MARKER}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: Bootstrap serves glyph assets used by bootstrap pages.
# 1. Confirm bootstrap mode is active.
# 2. Fetch `/glyphs.js` and assert it contains glyph bootstrap helper.
# 3. Fetch `/glyphs.css` and assert it contains background glyph styles.
def test_bootstrap_serves_glyph_assets(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    status, headers, body = _http_get(base_url, "/glyphs.js")
    assert status == 200, f"Expected /glyphs.js to be served in bootstrap mode, got {status}"
    assert "application/javascript" in headers.get("Content-Type", "")
    assert "sprinkleBackgroundGlyphs" in body

    status, headers, body = _http_get(base_url, "/glyphs.css")
    assert status == 200, f"Expected /glyphs.css to be served in bootstrap mode, got {status}"
    assert "text/css" in headers.get("Content-Type", "")
    assert ".bg-glyph-layer" in body


# Test: Three clear->blocked laser beam breaks in bootstrap should bump countdown to ~60s.
# 1. Enter bootstrap mode and read current countdown.
# 2. If countdown is above 60, wait until it drops below 60.
# 3. Inject two beam breaks and verify countdown did not increase.
# 4. Inject a third beam break.
# 5. Assert countdown becomes approximately 60 seconds.
def test_bootstrap_three_laser_beam_breaks_extend_countdown_to_sixty(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    countdown = _get_bootstrap_countdown_seconds(base_url)
    if countdown > 60:
        reached_under_sixty = _wait_until(
            lambda: _get_bootstrap_countdown_seconds(base_url) < 60,
            timeout_s=float(countdown + 5),
            poll_s=0.2,
        )
        assert reached_under_sixty, (
            "Countdown did not drop below 60 in time before laser-trigger test.\n"
            f"Initial countdown={countdown}\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

    pre_break_countdown = _get_bootstrap_countdown_seconds(base_url)
    pre_break_note = _get_bootstrap_extend_note_text(base_url)
    assert pre_break_note == BOOTSTRAP_EXTEND_NOTE_DEFAULT, (
        "Unexpected extension note before laser-trigger threshold.\n"
        f"note={pre_break_note}\n"
        f"expected={BOOTSTRAP_EXTEND_NOTE_DEFAULT}"
    )

    _inject_laser_beam_break(base_url)
    _inject_laser_beam_break(base_url)

    after_two_breaks = _get_bootstrap_countdown_seconds(base_url)
    after_two_breaks_note = _get_bootstrap_extend_note_text(base_url)
    assert after_two_breaks <= pre_break_countdown, (
        "Countdown increased before third laser beam break.\n"
        f"before={pre_break_countdown}, after_two={after_two_breaks}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
    assert after_two_breaks_note == BOOTSTRAP_EXTEND_NOTE_DEFAULT, (
        "Extension note changed before third laser beam break.\n"
        f"before={pre_break_note}\n"
        f"after_two={after_two_breaks_note}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _inject_laser_beam_break(base_url)

    bumped_to_sixtyish = _wait_until(
        lambda: 57 <= _get_bootstrap_countdown_seconds(base_url) <= 60,
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert bumped_to_sixtyish, (
        "Countdown did not bump to ~60 after third laser beam break.\n"
        f"pre_break={pre_break_countdown}, after_two={after_two_breaks}, "
        f"after_three={_get_bootstrap_countdown_seconds(base_url)}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    note_updated = _wait_until(
        lambda: _get_bootstrap_extend_note_text(base_url) == BOOTSTRAP_EXTEND_NOTE_EXTENDED,
        timeout_s=2.0,
        poll_s=0.1,
    )
    assert note_updated, (
        "Extension note did not update after third laser beam break.\n"
        f"before={pre_break_note}\n"
        f"after_two={after_two_breaks_note}\n"
        f"after_three={_get_bootstrap_extend_note_text(base_url)}\n"
        f"expected_after_three={BOOTSTRAP_EXTEND_NOTE_EXTENDED}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: `/skip` starts main app (no auth flow).
# 1. Confirm bootstrap mode is active.
# 2. Call `GET /skip` and verify skip page response.
# 3. Wait until main app is reachable.
# 4. Confirm `/skip` no longer serves bootstrap skip page.
def test_skip(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    # Before skip: confirm we are on bootstrap root page via unique HTML marker.
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    # Hit /skip and verify skip page response.
    status, headers, body = _http_get(base_url, "/skip")
    assert status == 200
    assert headers.get("Cache-Control") == "no-store"
    assert "text/html" in headers.get("Content-Type", "")
    assert "Starting main application" in body

    # Wait for main app handoff to complete.
    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after /skip.\nLog tail:\n{_tail_log(log_path)}"

    # /skip should no longer return the bootstrap skip page once main app is active.
    status, _, body = _http_get(base_url, "/skip")
    assert not (status == 200 and "Starting main application" in body)


# Test: `/skip` starts main app and then login redirect is enforced with auth.
# 1. Confirm bootstrap mode is active.
# 2. Call `GET /skip`.
# 3. Wait until main app is reachable.
# 4. Enable auth via `/security/config`.
# 5. Assert `/sounds/` redirects to `/login`.
def test_skip_with_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    # Start from bootstrap mode and trigger handoff with /skip.
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)
    status, _, _ = _http_get(base_url, "/skip")
    assert status == 200

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after /skip.\nLog tail:\n{_tail_log(log_path)}"

    _ensure_auth_enabled(base_url)

    # With auth enabled, UI entry route should redirect to /login.
    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")


# Test: `/skip` near countdown expiry (race window), no auth.
# 1. Confirm bootstrap mode is active.
# 2. Wait until countdown is in final second.
# 3. Call `GET /skip` during that race window.
# 4. Wait until main app is reachable.
# 5. Assert `/sounds/` is directly accessible (`200`).
# 6. Confirm `/skip` no longer serves bootstrap skip page.
def test_skip_race_no_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _wait_for_bootstrap_countdown_threshold(
        base_url=base_url,
        threshold_s=RACE_SKIP_COUNTDOWN_THRESHOLD_S,
        timeout_s=BOOT_TIMEOUT_S,
    )
    assert countdown_s is not None, (
        "Could not reach race window before bootstrap ended.\n"
        f"Threshold: <= {RACE_SKIP_COUNTDOWN_THRESHOLD_S}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    # Trigger /skip right before countdown expiry; response may vary at handoff edge.
    _http_get(base_url, "/skip")

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after near-expiry /skip.\nLog tail:\n{_tail_log(log_path)}"

    # No-auth variant: /sounds/ should be directly accessible after handoff.
    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200, (
        "Expected /sounds/ to be directly accessible (no auth) after near-expiry /skip.\n"
        f"Got status: {status}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    status, _, body = _http_get(base_url, "/skip")
    assert not (status == 200 and "Starting main application" in body)


# Test: `/skip` near countdown expiry (race window), with auth redirect.
# 1. Confirm bootstrap mode is active.
# 2. Wait until countdown is in final second.
# 3. Call `GET /skip` during that race window.
# 4. Wait until main app is reachable.
# 5. Enable auth via `/security/config`.
# 6. Assert `/sounds/` redirects to `/login`.
# 7. Confirm `/skip` no longer serves bootstrap skip page.
def test_skip_race_with_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _wait_for_bootstrap_countdown_threshold(
        base_url=base_url,
        threshold_s=RACE_SKIP_COUNTDOWN_THRESHOLD_S,
        timeout_s=BOOT_TIMEOUT_S,
    )
    assert countdown_s is not None, (
        "Could not reach race window before bootstrap ended.\n"
        f"Threshold: <= {RACE_SKIP_COUNTDOWN_THRESHOLD_S}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    # Trigger /skip right before countdown expiry; response may vary at handoff edge.
    _http_get(base_url, "/skip")

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after near-expiry /skip.\nLog tail:\n{_tail_log(log_path)}"

    _ensure_auth_enabled(base_url)

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")

    status, _, body = _http_get(base_url, "/skip")
    assert not (status == 200 and "Starting main application" in body)


# Test: Enter recovery mode, stay there past timer expiry, validate network info, then /skip to main app.
# 1. Confirm bootstrap mode is active and read countdown value.
# 2. Call `GET /recovery` and verify recovery page state.
# 3. Validate `/network/ips` JSON fields and format.
# 4. Wait past original countdown and assert we still remain in recovery.
# 5. Call `GET /skip` from recovery and verify main app takes over.
def test_recovery_mode_persists_and_skip_no_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)
    countdown_s = _extract_bootstrap_countdown_seconds(body)
    assert countdown_s is not None, "Could not parse bootstrap countdown before entering recovery."

    _enter_recovery_mode(base_url, log_path)

    status, headers, body = _http_get(base_url, "/")
    assert _is_recovery_bootstrap_page(status, headers, body)

    _assert_network_ips_payload(base_url)

    _assert_recovery_persists(base_url, duration_s=float(countdown_s + 2))

    status, headers, body = _http_get(base_url, "/skip")
    assert status == 200
    assert headers.get("Cache-Control") == "no-store"
    assert "Starting main application" in body

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after /skip from recovery.\nLog tail:\n{_tail_log(log_path)}"

def test_recovery_mode_persists_and_skip_with_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)
    countdown_s = _extract_bootstrap_countdown_seconds(body)
    assert countdown_s is not None, "Could not parse bootstrap countdown before entering recovery."

    _enter_recovery_mode(base_url, log_path)

    status, headers, body = _http_get(base_url, "/")
    assert _is_recovery_bootstrap_page(status, headers, body)

    _assert_network_ips_payload(base_url)

    _assert_recovery_persists(base_url, duration_s=float(countdown_s + 2))

    status, _, _ = _http_get(base_url, "/skip")
    assert status == 200

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after /skip from recovery.\nLog tail:\n{_tail_log(log_path)}"

    _ensure_auth_enabled(base_url)
    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")


# Test: Recovery race near countdown expiry.
# 1. Confirm bootstrap mode is active.
# 2. Wait until countdown reaches the final second.
# 3. Call `GET /recovery` right before expiry.
# 4. Wait a few seconds past expiry and assert recovery still holds.
# 5. Call `GET /skip` and verify main app handoff still works.
def test_recovery_race_near_expiry(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _wait_for_bootstrap_countdown_threshold(
        base_url=base_url,
        threshold_s=RACE_RECOVERY_COUNTDOWN_THRESHOLD_S,
        timeout_s=BOOT_TIMEOUT_S,
    )
    assert countdown_s is not None, (
        "Could not reach recovery race window before bootstrap ended.\n"
        f"Threshold: <= {RACE_RECOVERY_COUNTDOWN_THRESHOLD_S}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _enter_recovery_mode(base_url, log_path)

    # Ensure recovery still holds after the old countdown deadline would have passed.
    _assert_recovery_persists(base_url, duration_s=4.0)

    status, _, _ = _http_get(base_url, "/skip")
    assert status == 200
    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=MAIN_READY_TIMEOUT_S,
    )
    assert ready, f"Main app did not become ready after recovery race + /skip.\nLog tail:\n{_tail_log(log_path)}"


# Test: Recovery mode OTA endpoint accepts requests (safe probe).
# 1. Enter recovery mode and confirm recovery page state.
# 2. Send an intentionally invalid firmware OTA upload to `/update?partition=firmware`.
# 3. Verify OTA handler activity appears in logs (without requiring successful flash).
# 4. Confirm we still remain in recovery mode afterwards.
def test_recovery_ota_endpoint_probe(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _enter_recovery_mode(base_url, log_path)

    status, headers, body = _http_get(base_url, "/")
    assert _is_recovery_bootstrap_page(status, headers, body)

    start_pos = log_path.stat().st_size if log_path.exists() else 0
    response_status = None
    response_body = ""
    request_error = None

    # Safe probe: tiny invalid firmware payload should fail OTA quickly.
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

    ota_log_markers = [
        "Starting OTA OS",
        "OTA update failed",
        "received package is not fit len",
    ]
    ota_seen = _wait_until(
        lambda: _log_contains_any_since(log_path, start_pos, ota_log_markers),
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

    # Regardless of probe response shape, device should still remain in recovery mode.
    status, headers, body = _http_get(base_url, "/")
    assert _is_recovery_bootstrap_page(status, headers, body)


# Test: Recovery reset is safe and idempotent when already on defaults.
# 1. Start main app and capture baseline network/security config.
# 2. Restart back into bootstrap.
# 3. Enter recovery and call `POST /settings/reset`.
# 4. Leave recovery via `/skip` and wait for main app.
# 5. Assert network/security config matches baseline and auth stays disabled.
def test_recovery_reset_defaults_is_idempotent(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _skip_to_main_app(base_url, log_path)
    baseline_network = _get_network_config(base_url)
    baseline_security = _get_security_config(base_url)
    assert baseline_security.get("password_set") is False, (
        "Expected default auth to be disabled before idempotent reset test."
    )

    _restart_into_bootstrap(base_url, log_path)
    _enter_recovery_mode(base_url, log_path)
    _reset_settings_from_recovery(base_url)
    _skip_to_main_app(base_url, log_path)

    network_after_reset = _get_network_config(base_url)
    security_after_reset = _get_security_config(base_url)
    assert network_after_reset == baseline_network
    assert security_after_reset == baseline_security

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert status == 200, (
        "Expected /sounds/ to be directly accessible after reset on defaults.\n"
        f"status={status}, location={headers.get('Location')}"
    )


# Test: Recovery reset restores defaults after custom settings (including auth).
# 1. Start main app and capture baseline network/security config.
# 2. Configure non-default settings:
#    - Enable UI auth password.
#    - Update AP/STA settings when those features are enabled in this build.
# 3. Verify auth is active and (when applicable) network settings changed.
# 4. Restart into bootstrap using authenticated `/restart`.
# 5. Enter recovery and call `POST /settings/reset`.
# 6. Leave recovery via `/skip` and wait for main app.
# 7. Assert auth is disabled and network/security config is back to baseline defaults.
def test_recovery_reset_after_custom_settings(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _skip_to_main_app(base_url, log_path)
    baseline_network = _get_network_config(base_url)
    baseline_security = _get_security_config(base_url)
    assert baseline_security.get("password_set") is False, (
        "Expected auth to be disabled at baseline for reset-after-customization test."
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

    _restart_into_bootstrap(base_url, log_path, headers=auth_headers)
    _enter_recovery_mode(base_url, log_path)
    _reset_settings_from_recovery(base_url)
    _skip_to_main_app(base_url, log_path)

    network_after_reset = _get_network_config(base_url)
    security_after_reset = _get_security_config(base_url)
    assert security_after_reset == baseline_security
    assert network_after_reset == baseline_network

    status, headers, _ = _http_get(base_url, "/sounds/")
    assert status == 200, (
        "Expected /sounds/ to be accessible after recovery reset removed auth.\n"
        f"status={status}, location={headers.get('Location')}"
    )


# Test: Formatting storage in recovery works even when storage is empty.
# 1. Enter recovery mode.
# 2. Format once to establish an empty baseline.
# 3. Format again immediately (empty-storage case under test).
# 4. Verify device stays in recovery and can still hand off to main app via `/skip`.
def test_recovery_format_empty_storage_is_safe(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _enter_recovery_mode(base_url, log_path)
    _format_storage_from_recovery(base_url)
    _format_storage_from_recovery(base_url)

    status, headers, body = _http_get(base_url, "/")
    assert _is_recovery_bootstrap_page(status, headers, body)

    _skip_to_main_app(base_url, log_path)
    status, headers, _ = _http_get(base_url, "/sounds/")
    assert status == 200, (
        "Expected /sounds/ to stay accessible after empty-storage format flow.\n"
        f"status={status}, location={headers.get('Location')}"
    )


# Test: Formatting storage in recovery wipes uploaded sound files.
# 1. Start main app and upload sound files.
# 2. Confirm files are downloadable before format.
# 3. Restart to bootstrap, enter recovery, and call `POST /format`.
# 4. Return to main app via `/skip`.
# 5. Confirm uploaded files are no longer downloadable (`404`).
def test_recovery_format_wipes_uploaded_sounds(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _skip_to_main_app(base_url, log_path)

    unique = int(time.time() * 1000)
    filename_1 = f"fmt-{unique}-a.mp3"
    filename_2 = f"fmt-{unique}-b.mp3"
    payload = _test_mp3_bytes()
    _upload_sound_file(base_url, filename_1, payload)
    _upload_sound_file(base_url, filename_2, payload)

    _assert_sound_download_status(base_url, filename_1, 200)
    _assert_sound_download_status(base_url, filename_2, 200)

    _restart_into_bootstrap(base_url, log_path)
    _enter_recovery_mode(base_url, log_path)
    _format_storage_from_recovery(base_url)
    _skip_to_main_app(base_url, log_path)

    _assert_sound_download_status(base_url, filename_1, 404)
    _assert_sound_download_status(base_url, filename_2, 404)


# Test: Bootstrap countdown expires naturally (no auth flow).
# 1. Confirm bootstrap mode is active.
# 2. Read countdown value from bootstrap HTML.
# 3. Wait for main app handoff with timeout based on countdown.
# 4. Assert handoff did not happen too early.
# 5. Confirm `/skip` no longer serves bootstrap skip page.
def test_expire(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    # Confirm bootstrap page is active first.
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _extract_bootstrap_countdown_seconds(body)
    assert countdown_s is not None, (
        "Could not parse bootstrap countdown from page.\n"
        f"Body snippet:\n{body[:400]}"
    )

    # Allow render/transport jitter so this does not fail on second-boundary races.
    min_elapsed_s = max(0, countdown_s - 2)
    timeout_s = max(5.0, float(countdown_s + 20))
    started = time.time()

    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=timeout_s,
    )
    elapsed_s = time.time() - started

    assert ready, (
        "Main app did not become ready after bootstrap countdown expired.\n"
        f"Countdown from page: {countdown_s}s\n"
        f"Waited: {elapsed_s:.1f}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
    assert elapsed_s >= min_elapsed_s, (
        "Main app became ready too early relative to bootstrap countdown.\n"
        f"Countdown from page: {countdown_s}s\n"
        f"Elapsed: {elapsed_s:.1f}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    # Bootstrap endpoint should no longer serve the skip page once main app is active.
    status, _, body = _http_get(base_url, "/skip")
    assert not (status == 200 and "Starting main application" in body)

# Test: Bootstrap countdown expires naturally and auth redirect is enforced.
# 1. Confirm bootstrap mode is active.
# 2. Read countdown value from bootstrap HTML.
# 3. Wait for main app handoff with timeout based on countdown.
# 4. Assert handoff did not happen too early.
# 5. Enable auth via `/security/config`.
# 6. Assert `/sounds/` redirects to `/login`.
def test_expire_with_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    # Confirm bootstrap page is active first.
    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _extract_bootstrap_countdown_seconds(body)
    assert countdown_s is not None, (
        "Could not parse bootstrap countdown from page.\n"
        f"Body snippet:\n{body[:400]}"
    )

    min_elapsed_s = max(0, countdown_s - 2)
    timeout_s = max(5.0, float(countdown_s + 20))
    started = time.time()
    ready = _wait_until(
        lambda: _is_main_app_ready(base_url),
        timeout_s=timeout_s,
    )
    elapsed_s = time.time() - started

    assert ready, (
        "Main app did not become ready after bootstrap countdown expired.\n"
        f"Countdown from page: {countdown_s}s\n"
        f"Waited: {elapsed_s:.1f}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
    assert elapsed_s >= min_elapsed_s, (
        "Main app became ready too early relative to bootstrap countdown.\n"
        f"Countdown from page: {countdown_s}s\n"
        f"Elapsed: {elapsed_s:.1f}s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _ensure_auth_enabled(base_url)

    # With auth enabled, UI entry route should redirect to /login.
    status, headers, _ = _http_get(base_url, "/sounds/")
    assert _is_login_redirect(status, headers, "/sounds/")
