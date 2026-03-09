from __future__ import annotations

import json
import time
from urllib.parse import urlencode

import pytest

try:
    from tests.integration.integration_helpers import (
        _http_get,
        _http_post_json,
        _http_request,
        _log_contains_any_since,
        _restart_into_bootstrap,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_until,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        _http_get,
        _http_post_json,
        _http_request,
        _log_contains_any_since,
        _restart_into_bootstrap,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_until,
        qemu_bootstrap_instance,
    )


AMP_MUTED_LEVEL = 1
AMP_UNMUTED_LEVEL = 0
DAC_MUTED_LEVEL = 0
DAC_UNMUTED_LEVEL = 1
PANIC_LOG_MARKERS = [
    "Guru Meditation Error",
    "panic_abort",
    "assert failed",
    "Backtrace:",
]
DEFAULT_SOUND_FILENAME = "default.mp3"
AUDIO_CONFIG_TEST_ONLY_KEY = "test_only"
AUDIO_CONFIG_TEST_ONLY_FIELDS = (
    "laser_debounce_ms",
    "hall_debounce_ms",
    "laser_trigger_cooldown_ms",
)
LASER_DEBOUNCE_DEFAULT_MS = 30
LASER_DEBOUNCE_MIN_MS = 0
LASER_DEBOUNCE_MAX_MS = 1000


@pytest.fixture
def qemu_mainapp_instance(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    _skip_to_main_app(base_url, log_path)
    return qemu_bootstrap_instance


def _unique_name(prefix: str) -> str:
    return f"{prefix}-{int(time.time() * 1000)}"


def _json_object(body: str, context: str) -> dict:
    try:
        value = json.loads(body)
    except json.JSONDecodeError as exc:
        pytest.fail(f"Failed to decode JSON from {context}: {exc}\nBody:\n{body}")
    assert isinstance(value, dict), f"Expected object JSON from {context}, got {type(value).__name__}"
    return value


def _test_gpio_level(base_url: str, name: str) -> int:
    status, _, body = _http_get(base_url, f"/test/gpio/{name}")
    assert status == 200, f"GET /test/gpio/{name} failed. status={status}, body={body}"
    payload = _json_object(body, f"GET /test/gpio/{name}")
    level = payload.get("level")
    assert level in (0, 1), f"Unexpected level for /test/gpio/{name}: {level}"
    return level


def _outputs_are_muted(base_url: str) -> bool:
    amp = _test_gpio_level(base_url, "amp-mute")
    dac = _test_gpio_level(base_url, "dac-mute")
    return amp == AMP_MUTED_LEVEL and dac == DAC_MUTED_LEVEL


def _outputs_are_unmuted(base_url: str) -> bool:
    amp = _test_gpio_level(base_url, "amp-mute")
    dac = _test_gpio_level(base_url, "dac-mute")
    return amp == AMP_UNMUTED_LEVEL and dac == DAC_UNMUTED_LEVEL


def _audio_test_state(base_url: str) -> dict:
    status, _, body = _http_get(base_url, "/audio/test")
    assert status == 200, f"GET /audio/test failed. status={status}, body={body}"
    return _json_object(body, "GET /audio/test")


def _audio_test_post(
    base_url: str,
    action: str,
    freq: float | None = None,
    volume: float | None = None,
):
    params: dict[str, str] = {"action": action}
    if freq is not None:
        params["freq"] = str(freq)
    if volume is not None:
        params["volume"] = str(volume)
    query = urlencode(params)
    return _http_request(
        base_url=base_url,
        method="POST",
        path=f"/audio/test?{query}",
        timeout_s=3.0,
        data=b"",
    )


def _audio_playback_start(base_url: str, filename: str, timeout_s: float = 3.0):
    return _http_request(
        base_url=base_url,
        method="POST",
        path=f"/audio/playback?name={filename}",
        timeout_s=timeout_s,
        data=b"",
    )


def _audio_playback_stop(base_url: str, timeout_s: float = 3.0):
    return _http_request(
        base_url=base_url,
        method="POST",
        path="/audio/playback?action=stop",
        timeout_s=timeout_s,
        data=b"",
    )


def _audio_playback_state(base_url: str) -> dict:
    status, _, body = _http_get(base_url, "/audio/playback")
    assert status == 200, f"GET /audio/playback failed. status={status}, body={body}"
    return _json_object(body, "GET /audio/playback")


def _gpio_state(base_url: str) -> dict:
    status, _, body = _http_get(base_url, "/gpio/state")
    assert status == 200, f"GET /gpio/state failed. status={status}, body={body}"
    return _json_object(body, "GET /gpio/state")


def _set_sound_meta(base_url: str, filename: str, payload: dict):
    return _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/file-meta/{filename}",
        timeout_s=4.0,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )


def _get_boot_config(base_url: str) -> dict:
    status, _, body = _http_get(base_url, "/boot/config")
    assert status == 200, f"GET /boot/config failed. status={status}, body={body}"
    return _json_object(body, "GET /boot/config")


def _get_audio_config(base_url: str) -> dict:
    status, _, body = _http_get(base_url, "/audio/config")
    assert status == 200, f"GET /audio/config failed. status={status}, body={body}"
    return _json_object(body, "GET /audio/config")


def _set_audio_config(base_url: str, payload: dict) -> dict:
    status, _, body = _http_post_json(
        base_url=base_url,
        path="/audio/config",
        payload=payload,
        timeout_s=4.0,
    )
    assert status == 200, f"POST /audio/config failed. status={status}, body={body}"
    return _json_object(body, "POST /audio/config")


def _set_audio_test_only_config(base_url: str, **updates) -> dict:
    current = _get_audio_config(base_url)
    current_test_only = current.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(current_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} in /audio/config, "
        f"got {current_test_only!r}"
    )
    next_test_only = dict(current_test_only)
    next_test_only.update(updates)
    return _set_audio_config(base_url, {AUDIO_CONFIG_TEST_ONLY_KEY: next_test_only})


def _set_laser_debounce_ms(base_url: str, debounce_ms: int) -> dict:
    return _set_audio_test_only_config(base_url, laser_debounce_ms=debounce_ms)


def _set_boot_config(base_url: str, payload: dict) -> dict:
    status, _, body = _http_post_json(
        base_url=base_url,
        path="/boot/config",
        payload=payload,
        timeout_s=4.0,
    )
    assert status == 200, f"POST /boot/config failed. status={status}, body={body}"
    return _json_object(body, "POST /boot/config")


def _volume_pct(state: dict) -> float:
    return float(state.get("volume_pct", -1.0))


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


def _trigger_laser_playback(base_url: str):
    _set_test_gpio_level(base_url, "laser", 0)
    _set_test_gpio_level(base_url, "laser", 1)


def _trigger_laser_playback_burst(base_url: str, count: int, interval_s: float):
    assert count > 0, "Laser playback burst must trigger at least once."
    interval_ms = max(0, int(round(interval_s * 1000.0)))
    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/test/gpio/laser-burst?count={count}&interval_ms={interval_ms}",
        timeout_s=max(12.0, (count * interval_s) + 6.0),
        data=b"",
    )
    assert status == 200, (
        f"Failed to trigger /test/gpio/laser-burst?count={count}&interval_ms={interval_ms}. "
        f"status={status}, body={body}"
    )
    payload = _json_object(body, "POST /test/gpio/laser-burst")
    assert payload.get("count") == count
    assert payload.get("interval_ms") == interval_ms
    assert payload.get("level") == 1


def _log_text_since(log_path, start_pos: int = 0) -> str:
    if not log_path.exists():
        return ""
    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        if start_pos > 0:
            f.seek(start_pos)
        return f.read()


def _log_since_contains_all(log_path, start_pos: int, markers: list[str]) -> bool:
    text = _log_text_since(log_path, start_pos)
    return all(marker in text for marker in markers)


def _log_count_since(log_path, start_pos: int, marker: str) -> int:
    return _log_text_since(log_path, start_pos).count(marker)


def _playback_stays_inactive_for(base_url: str, duration_s: float, poll_s: float = 0.05) -> bool:
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        if bool(_audio_playback_state(base_url).get("active")):
            return False
        time.sleep(poll_s)
    return True


def _assert_no_panic_since(log_path, start_pos: int):
    has_panic = _log_contains_any_since(log_path, start_pos, PANIC_LOG_MARKERS)
    assert not has_panic, f"Detected panic markers after playback restart spam.\nLog tail:\n{_tail_log(log_path)}"


def _wait_for_playback_idle(base_url: str, timeout_s: float = 3.0) -> bool:
    return _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=timeout_s,
        poll_s=0.1,
    )


def _stop_playback_if_active(base_url: str):
    if not bool(_audio_playback_state(base_url).get("active")):
        return
    status, _, body = _audio_playback_stop(base_url, timeout_s=8.0)
    assert status == 200, f"Failed to stop playback cleanup. body={body}"
    stopped = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert stopped, "Playback did not clear after cleanup stop."


def _configure_single_laser_candidate(base_url: str, filename: str):
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _set_sound_meta(base_url, DEFAULT_SOUND_FILENAME, {"probability": 0, "enabled": True})
    assert status == 200, f"Failed to remove default sound from laser selection. body={body}"
    assert body == "OK"

    status, _, body = _set_sound_meta(
        base_url,
        filename,
        {"enabled": True, "probability": 100, "volume": 100},
    )
    assert status == 200, f"Failed to configure deterministic laser candidate. body={body}"
    assert body == "OK"


# Test: Device boots into main app with DAC + AMP muted.
# 1. Start from main app mode.
# 2. Poll test GPIO mute endpoints.
# 3. Assert DAC/AMP are in muted levels.
def test_boot_starts_with_dac_and_amp_muted(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, "Expected DAC+AMP to be muted after boot into main app."


# Test: Entering the main app starts playback of the built-in default sound by default.
# 1. Start from bootstrap mode on a fresh device.
# 2. Capture the current log position.
# 3. Leave bootstrap via `/skip`.
# 4. Assert logs show playback starting for `default.mp3`.
def test_entering_main_app_plays_default_sound_by_default(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _skip_to_main_app(base_url, log_path)

    playback_seen = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Starting playback: {DEFAULT_SOUND_FILENAME}"],
        ),
        timeout_s=5.0,
        poll_s=0.1,
    )
    assert playback_seen, (
        "Expected the built-in default sound to play when entering the main app.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: Entering the main app does not play the default sound after boot sound is disabled.
# 1. Start from bootstrap mode and enter the main app once.
# 2. Disable boot sound through `POST /boot/config`.
# 3. Restart back to bootstrap, capture the new log position, then leave bootstrap again.
# 4. Assert no playback start for `default.mp3` appears after the second handoff.
def test_entering_main_app_does_not_play_default_sound_when_boot_sound_disabled(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    _skip_to_main_app(base_url, log_path)

    boot_cfg = _get_boot_config(base_url)
    assert boot_cfg.get("boot_sound_enabled") is True

    updated_cfg = _set_boot_config(base_url, {"boot_sound_enabled": False})
    assert updated_cfg.get("boot_sound_enabled") is False

    _restart_into_bootstrap(base_url, log_path)

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _skip_to_main_app(base_url, log_path)

    playback_seen = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Starting playback: {DEFAULT_SOUND_FILENAME}"],
        ),
        timeout_s=5.0,
        poll_s=0.1,
    )
    assert not playback_seen, (
        "Did not expect the default boot sound to play after disabling it.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: `/audio/config` updates lid-closed/open playback volumes and logs the change.
# 1. Start from main app mode and capture baseline audio config.
# 2. POST new lid-closed and lid-open volume values.
# 3. Verify the response and a fresh GET both reflect the new values.
# 4. Assert the update is recorded in logs.
def test_audio_config_updates_lid_volumes_and_logs_change(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    baseline = _get_audio_config(base_url)
    for key in ("lid_closed_volume_pct", "lid_open_volume_pct"):
        value = baseline.get(key)
        assert isinstance(value, int), f"Expected integer {key} in /audio/config, got {value!r}"
        assert 0 <= value <= 100, f"Expected {key} within 0-100, got {value}"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    updated = _set_audio_config(
        base_url,
        {"lid_closed_volume_pct": 17, "lid_open_volume_pct": 63},
    )
    assert updated.get("lid_closed_volume_pct") == 17
    assert updated.get("lid_open_volume_pct") == 63

    readback = _get_audio_config(base_url)
    assert readback.get("lid_closed_volume_pct") == 17
    assert readback.get("lid_open_volume_pct") == 63

    config_logged = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Audio config updated: lid_closed=17 lid_open=63"],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert config_logged, (
        "Expected /audio/config update to be reflected in logs.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: `/audio/config` exposes writable test-only debounce settings.
# 1. Start from main app mode and capture baseline audio config.
# 2. Assert the test-only debounce object is present with integer timing fields.
# 3. POST new test-only debounce values.
# 4. Verify the response and a fresh GET both reflect the new debounce values.
# 5. Verify regular lid-volume settings remain unchanged when omitted from the update.
def test_audio_config_updates_test_only_debounce_settings(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    baseline = _get_audio_config(base_url)
    baseline_test_only = baseline.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(baseline_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} in /audio/config, "
        f"got {baseline_test_only!r}"
    )
    for key in AUDIO_CONFIG_TEST_ONLY_FIELDS:
        value = baseline_test_only.get(key)
        assert isinstance(value, int), (
            f"Expected integer {AUDIO_CONFIG_TEST_ONLY_KEY}.{key} in /audio/config, got {value!r}"
        )
        assert value >= 0, (
            f"Expected non-negative {AUDIO_CONFIG_TEST_ONLY_KEY}.{key} in /audio/config, got {value}"
        )

    new_test_only = {
        "laser_debounce_ms": 25,
        "hall_debounce_ms": 750,
        "laser_trigger_cooldown_ms": 125,
    }
    updated = _set_audio_config(base_url, {AUDIO_CONFIG_TEST_ONLY_KEY: new_test_only})
    updated_test_only = updated.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(updated_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} after POST /audio/config, "
        f"got {updated_test_only!r}"
    )
    for key, expected in new_test_only.items():
        assert updated_test_only.get(key) == expected, (
            f"Expected {AUDIO_CONFIG_TEST_ONLY_KEY}.{key}={expected} after POST /audio/config, "
            f"got {updated_test_only.get(key)!r}"
        )

    assert updated.get("lid_closed_volume_pct") == baseline.get("lid_closed_volume_pct")
    assert updated.get("lid_open_volume_pct") == baseline.get("lid_open_volume_pct")

    readback = _get_audio_config(base_url)
    readback_test_only = readback.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(readback_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} on GET /audio/config readback, "
        f"got {readback_test_only!r}"
    )
    for key, expected in new_test_only.items():
        assert readback_test_only.get(key) == expected, (
            f"Expected readback {AUDIO_CONFIG_TEST_ONLY_KEY}.{key}={expected}, "
            f"got {readback_test_only.get(key)!r}"
        )


# Test: `/audio/config` reports `30ms` as the default laser debounce.
# 1. Start from main app mode and read `/audio/config`.
# 2. Assert the test-only object is present.
# 3. Assert `test_only.laser_debounce_ms` equals the default `30`.
def test_audio_config_reports_default_laser_debounce_ms(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    config = _get_audio_config(base_url)
    test_only = config.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} in /audio/config, got {test_only!r}"
    )
    assert test_only.get("laser_debounce_ms") == LASER_DEBOUNCE_DEFAULT_MS, (
        "Expected default laser debounce to be 30ms.\n"
        f"Config: {config}"
    )


# Test: `/audio/config` rejects out-of-range laser debounce values.
# 1. Start from main app mode and capture the baseline laser debounce.
# 2. Attempt to write laser debounce values below `0` and above `1000`.
# 3. Assert each request is rejected with `400`.
# 4. Assert the stored laser debounce remains unchanged afterwards.
def test_audio_config_rejects_out_of_range_laser_debounce_values(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    baseline = _get_audio_config(base_url)
    baseline_test_only = baseline.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(baseline_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} in /audio/config, got {baseline_test_only!r}"
    )
    baseline_laser_debounce = baseline_test_only.get("laser_debounce_ms")
    assert isinstance(baseline_laser_debounce, int), (
        f"Expected integer {AUDIO_CONFIG_TEST_ONLY_KEY}.laser_debounce_ms, got {baseline_laser_debounce!r}"
    )

    for bad_value in (LASER_DEBOUNCE_MIN_MS - 1, LASER_DEBOUNCE_MAX_MS + 1):
        payload = {
            AUDIO_CONFIG_TEST_ONLY_KEY: {
                **baseline_test_only,
                "laser_debounce_ms": bad_value,
            }
        }
        status, _, body = _http_post_json(
            base_url=base_url,
            path="/audio/config",
            payload=payload,
            timeout_s=4.0,
        )
        assert status == 400, (
            f"Expected 400 for out-of-range laser debounce value {bad_value}. "
            f"body={body}"
        )

        readback = _get_audio_config(base_url)
        readback_test_only = readback.get(AUDIO_CONFIG_TEST_ONLY_KEY)
        assert isinstance(readback_test_only, dict), (
            f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} on readback, got {readback_test_only!r}"
        )
        assert readback_test_only.get("laser_debounce_ms") == baseline_laser_debounce, (
            f"Out-of-range laser debounce value {bad_value} should not change stored config.\n"
            f"Readback: {readback}"
        )


# Test: Laser GPIO history remains raw even when playback debouncing is enabled.
# 1. Start from main app mode and set laser debounce to a non-zero value.
# 2. Clear the current `/gpio/state` laser history.
# 3. Toggle the injected laser GPIO rapidly several times within the debounce period.
# 4. Assert `/gpio/state` still reports every raw edge in order.
def test_laser_gpio_history_is_exempt_from_playback_debounce(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    updated = _set_laser_debounce_ms(base_url, 200)
    updated_test_only = updated.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(updated_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} after debounce update, got {updated_test_only!r}"
    )
    assert updated_test_only.get("laser_debounce_ms") == 200

    status, _, body = _set_sound_meta(base_url, DEFAULT_SOUND_FILENAME, {"probability": 0, "enabled": True})
    assert status == 200, f"Failed to disable default sound for raw-history test. body={body}"
    assert body == "OK"

    _set_test_gpio_level(base_url, "laser", 0)
    _gpio_state(base_url)

    expected_levels = [1, 0, 1, 0, 1]
    for level in expected_levels:
        _set_test_gpio_level(base_url, "laser", level)
        time.sleep(0.01)

    state = _gpio_state(base_url)
    laser = state.get("laser")
    assert isinstance(laser, dict), f"Expected laser object in /gpio/state, got {laser!r}"
    events = laser.get("events")
    assert isinstance(events, list), f"Expected laser.events list in /gpio/state, got {events!r}"
    assert len(events) == len(expected_levels), (
        "Expected laser history to contain every raw GPIO event despite debounce.\n"
        f"State: {state}"
    )

    observed_levels = [int(evt.get("level", -1)) for evt in events]
    assert observed_levels == expected_levels, (
        "Laser history should reflect the raw edge sequence even while playback debouncing is enabled.\n"
        f"Observed: {observed_levels}\n"
        f"Expected: {expected_levels}"
    )


# Test: Laser playback is guarded by the configured debounce value.
# 1. Start from main app mode, wait for startup playback to clear, and set a large laser debounce.
# 2. Make one uploaded MP3 the only laser playback candidate.
# 3. Trigger the laser repeatedly within the debounce period.
# 4. Assert only one real playback start is logged.
def test_laser_playback_is_guarded_by_laser_debounce(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, f"Startup playback did not clear before debounce guard test.\nLog tail:\n{_tail_log(log_path)}"

    updated = _set_laser_debounce_ms(base_url, 200)
    updated_test_only = updated.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(updated_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} after debounce update, got {updated_test_only!r}"
    )
    assert updated_test_only.get("laser_debounce_ms") == 200

    filename = f"{_unique_name('laser-debounce-guard-6165ms')}.mp3"
    _configure_single_laser_candidate(base_url, filename)

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _trigger_laser_playback_burst(base_url, count=3, interval_s=0.05)

    first_start_seen = _wait_until(
        lambda: _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}") >= 1,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert first_start_seen, (
        "Expected at least one playback start after the laser burst.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    time.sleep(0.5)
    start_count = _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}")
    assert start_count == 1, (
        "Expected laser debounce to suppress repeated playback starts within the debounce window.\n"
        f"Observed start count: {start_count}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _stop_playback_if_active(base_url)


# Test: A laser debounce value of `0` disables playback guarding.
# 1. Start from main app mode, wait for startup playback to clear, and set laser debounce to `0`.
# 2. Make one uploaded MP3 the only laser playback candidate.
# 3. Trigger the laser repeatedly with short gaps.
# 4. Assert more than one playback start is logged.
def test_zero_laser_debounce_disables_playback_guarding(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before zero-debounce playback test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    updated = _set_laser_debounce_ms(base_url, 0)
    updated_test_only = updated.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(updated_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} after debounce update, got {updated_test_only!r}"
    )
    assert updated_test_only.get("laser_debounce_ms") == 0

    filename = f"{_unique_name('laser-debounce-zero-6165ms')}.mp3"
    _configure_single_laser_candidate(base_url, filename)

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _trigger_laser_playback_burst(base_url, count=3, interval_s=0.08)

    multiple_starts_seen = _wait_until(
        lambda: _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}") >= 2,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert multiple_starts_seen, (
        "Expected zero laser debounce to allow repeated playback starts from rapid laser toggles.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _stop_playback_if_active(base_url)


# Test: Changing the laser debounce value changes playback restart behavior.
# 1. Start from main app mode and make one uploaded MP3 the only laser playback candidate.
# 2. With a high debounce, trigger the same laser burst and record the playback-start count.
# 3. Stop playback, lower the debounce, and trigger the identical burst again.
# 4. Assert the lower debounce produces more playback starts than the higher debounce.
def test_changing_laser_debounce_changes_playback_behavior(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before debounce-change behavior test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('laser-debounce-change-6165ms')}.mp3"
    _configure_single_laser_candidate(base_url, filename)

    updated = _set_laser_debounce_ms(base_url, 200)
    updated_test_only = updated.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(updated_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} after high-debounce update, got {updated_test_only!r}"
    )
    assert updated_test_only.get("laser_debounce_ms") == 200

    high_log_start = log_path.stat().st_size if log_path.exists() else 0
    _trigger_laser_playback_burst(base_url, count=3, interval_s=0.08)

    high_start_seen = _wait_until(
        lambda: _log_count_since(log_path, high_log_start, f"Starting playback: {filename}") >= 1,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert high_start_seen, (
        "Expected at least one playback start during the high-debounce burst.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    time.sleep(0.5)
    high_count = _log_count_since(log_path, high_log_start, f"Starting playback: {filename}")
    _stop_playback_if_active(base_url)

    updated = _set_laser_debounce_ms(base_url, 50)
    updated_test_only = updated.get(AUDIO_CONFIG_TEST_ONLY_KEY)
    assert isinstance(updated_test_only, dict), (
        f"Expected object {AUDIO_CONFIG_TEST_ONLY_KEY!r} after low-debounce update, got {updated_test_only!r}"
    )
    assert updated_test_only.get("laser_debounce_ms") == 50

    low_log_start = log_path.stat().st_size if log_path.exists() else 0
    _trigger_laser_playback_burst(base_url, count=3, interval_s=0.08)

    low_starts_seen = _wait_until(
        lambda: _log_count_since(log_path, low_log_start, f"Starting playback: {filename}") >= 2,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert low_starts_seen, (
        "Expected the lower laser debounce to allow more playback restarts for the same laser burst.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    low_count = _log_count_since(log_path, low_log_start, f"Starting playback: {filename}")
    assert high_count == 1, (
        f"Expected high laser debounce to collapse the burst to one playback start, got {high_count}.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
    assert low_count > high_count, (
        "Expected lowering the laser debounce to increase playback starts for the same burst.\n"
        f"high_count={high_count} low_count={low_count}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _stop_playback_if_active(base_url)


# Test: Audio test (not playing) accepts frequency and volume updates.
# 1. Start from main app mode and assert test is not running.
# 2. Update frequency via `POST /audio/test?action=update&freq=...`.
# 3. Verify `freq_hz` reflects new value.
# 4. Update volume via `POST /audio/test?action=update&volume=...`.
# 5. Verify `volume_pct` reflects new value.
def test_audio_test_idle_allows_frequency_and_volume_updates(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    state = _audio_test_state(base_url)
    assert state.get("running") is False
    assert state.get("sweep_running") is False

    status, _, body = _audio_test_post(base_url, action="update", freq=1234.0)
    assert status == 200, f"Frequency update failed. body={body}"
    state = _json_object(body, "POST /audio/test update freq")
    assert abs(float(state.get("freq_hz", 0.0)) - 1234.0) <= 1.0
    assert state.get("running") is False

    status, _, body = _audio_test_post(base_url, action="update", volume=37.0)
    assert status == 200, f"Volume update failed. body={body}"
    state = _json_object(body, "POST /audio/test update volume")
    assert abs(_volume_pct(state) - 37.0) <= 2.0
    assert state.get("running") is False


# Test: Sweep keeps the requested UI volume percentage stable.
# 1. Start from main app mode and set test volume to a non-exact hardware step like `52%`.
# 2. Start sweep and assert the returned `volume_pct` still reports `52`.
# 3. Poll `GET /audio/test` during the sweep and assert the same percentage is preserved.
# 4. Stop the sweep for cleanup.
def test_audio_test_sweep_preserves_requested_volume_pct(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, _, body = _audio_test_post(base_url, action="update", volume=52.0)
    assert status == 200, f"Volume update failed before sweep. body={body}"
    state = _json_object(body, "POST /audio/test update volume before sweep")
    assert abs(_volume_pct(state) - 52.0) <= 0.1

    status, _, body = _audio_test_post(base_url, action="sweep")
    assert status == 200, f"Failed to start sweep. body={body}"
    state = _json_object(body, "POST /audio/test sweep")
    assert state.get("sweep_running") is True
    assert abs(_volume_pct(state) - 52.0) <= 0.1

    sweep_running = _wait_until(
        lambda: bool(_audio_test_state(base_url).get("sweep_running")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert sweep_running, "Sweep did not report running=true after start."

    polled_state = _audio_test_state(base_url)
    assert abs(_volume_pct(polled_state) - 52.0) <= 0.1

    status, _, body = _audio_test_post(base_url, action="stop")
    assert status == 200, f"Failed to stop sweep cleanup. body={body}"


# Test: Audio test (not playing) rejects invalid frequency and volume.
# 1. Start from main app mode.
# 2. Send invalid/out-of-range frequency updates and assert `400`.
# 3. Send invalid/out-of-range volume updates and assert `400`.
def test_audio_test_idle_rejects_invalid_frequency_and_volume(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    for bad_freq in ("abc", "0", "9001.0"):
        status, _, body = _audio_test_post(base_url, action="update", freq=bad_freq)
        assert status == 400, f"Expected 400 for bad freq={bad_freq}. body={body}"
        assert "Invalid freq" in body

    for bad_volume in ("abc", "-1", "101"):
        status, _, body = _audio_test_post(base_url, action="update", volume=bad_volume)
        assert status == 400, f"Expected 400 for bad volume={bad_volume}. body={body}"
        assert "Invalid volume" in body


# Test: Audio test while playing unmutes outputs, supports live updates, and blocks sweep/playback.
# 1. Upload real MP3 for playback-block check.
# 2. Start tone via `POST /audio/test?action=start`.
# 3. Assert tone is running and DAC/AMP unmute.
# 4. Update frequency+volume while running and verify reflected values.
# 5. Assert sweep trigger is rejected while tone is running.
# 6. Assert playback start is rejected while tone is running.
# 7. Stop tone and assert DAC/AMP return to muted levels.
def test_audio_test_playing_updates_and_blocks_sweep_and_playback(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    filename = f"{_unique_name('tone-block')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _audio_test_post(base_url, action="start", freq=440.0, volume=25.0)
    assert status == 200, f"Failed to start tone. body={body}"

    running = _wait_until(
        lambda: bool(_audio_test_state(base_url).get("running")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert running, f"Tone did not report running=true.\nLog tail:\n{_tail_log(log_path)}"

    unmuted = _wait_until(lambda: _outputs_are_unmuted(base_url), timeout_s=3.0, poll_s=0.1)
    assert unmuted, f"DAC/AMP did not unmute while tone running.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _audio_test_post(base_url, action="update", freq=880.0, volume=40.0)
    assert status == 200, f"Tone update failed. body={body}"
    state = _json_object(body, "POST /audio/test update while running")
    assert state.get("running") is True
    assert abs(float(state.get("freq_hz", 0.0)) - 880.0) <= 1.0
    assert abs(_volume_pct(state) - 40.0) <= 2.0

    status, _, body = _audio_test_post(base_url, action="sweep")
    assert status == 409, f"Expected sweep blocked during tone. body={body}"
    assert "Test tone in progress" in body

    status, _, body = _audio_playback_start(base_url, filename)
    assert status == 409, f"Expected playback blocked during tone. body={body}"
    assert "Audio test in progress" in body

    status, _, body = _audio_test_post(base_url, action="stop")
    assert status == 200, f"Failed to stop tone. body={body}"

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, f"DAC/AMP did not return to muted after tone stop.\nLog tail:\n{_tail_log(log_path)}"


# Test: Sweep unmutes/mutes correctly, blocks other audio, and unblocks after completion.
# 1. Upload real MP3 for playback-block check.
# 2. Start sweep via `POST /audio/test?action=sweep`.
# 3. Assert sweep is running and DAC/AMP unmute.
# 4. Assert tone start is rejected during sweep.
# 5. Assert playback start is rejected during sweep.
# 6. Verify sweep runs for a meaningful duration; accept natural completion or stop fallback.
# 7. Assert DAC/AMP are muted after sweep ends.
# 8. Assert tone start and playback start are allowed again.
def test_sweep_blocks_other_audio_until_done_then_unblocks(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    filename = f"{_unique_name('sweep-block')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    t_start = time.monotonic()
    status, _, body = _audio_test_post(base_url, action="sweep")
    assert status == 200, f"Failed to start sweep. body={body}"

    sweep_running = _wait_until(
        lambda: bool(_audio_test_state(base_url).get("sweep_running")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert sweep_running, f"Sweep did not report running=true.\nLog tail:\n{_tail_log(log_path)}"

    unmuted = _wait_until(lambda: _outputs_are_unmuted(base_url), timeout_s=3.0, poll_s=0.1)
    assert unmuted, f"DAC/AMP did not unmute during sweep.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _audio_test_post(base_url, action="start", freq=330.0, volume=25.0)
    assert status == 409, f"Expected tone start blocked during sweep. body={body}"
    assert "Sweep in progress" in body

    status, _, body = _audio_playback_start(base_url, filename)
    assert status == 409, f"Expected playback blocked during sweep. body={body}"
    assert "Sweep in progress" in body

    elapsed_min_ok = _wait_until(
        lambda: (time.monotonic() - t_start) >= 5.0,
        timeout_s=8.0,
        poll_s=0.1,
    )
    assert elapsed_min_ok, "Sweep did not stay active for a meaningful duration."

    sweep_done = _wait_until(
        lambda: not bool(_audio_test_state(base_url).get("sweep_running")),
        timeout_s=8.0,
        poll_s=0.1,
    )
    if not sweep_done:
        # QEMU audio sinks can stall draining; stop explicitly so unblock semantics are still verified.
        status, _, body = _audio_test_post(base_url, action="stop")
        assert status == 200, f"Failed to stop sweep fallback. body={body}"
        sweep_done = _wait_until(
            lambda: not bool(_audio_test_state(base_url).get("sweep_running")),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert sweep_done, f"Sweep did not clear after stop fallback.\nLog tail:\n{_tail_log(log_path)}"

    elapsed_s = time.monotonic() - t_start
    assert elapsed_s >= 5.0, f"Sweep ended too quickly: {elapsed_s:.2f}s"

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, f"DAC/AMP did not return to muted after sweep.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _audio_test_post(base_url, action="start", freq=440.0, volume=20.0)
    assert status == 200, f"Tone should be allowed after sweep. body={body}"
    status, _, body = _audio_test_post(base_url, action="stop")
    assert status == 200, f"Tone stop failed after sweep. body={body}"
    tone_stopped = _wait_until(
        lambda: not bool(_audio_test_state(base_url).get("running")),
        timeout_s=2.0,
        poll_s=0.1,
    )
    assert tone_stopped, f"Tone did not clear after stop.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _audio_playback_start(base_url, filename, timeout_s=8.0)
    assert status == 200, f"Playback should be allowed after sweep. body={body}"
    status, _, body = _audio_playback_stop(base_url, timeout_s=8.0)
    assert status == 200, f"Playback stop failed after sweep. body={body}"


# Test: `GET /audio/playback` reports the active file while playback is running.
# 1. Upload real MP3 fixture (~6s).
# 2. Start playback and poll `GET /audio/playback` until `active=true`.
# 3. Assert `file` reports the requested filename and `skipped` is empty.
# 4. Assert DAC/AMP unmute during active playback and test/sweep are blocked.
# 5. Verify playback remains active for meaningful time; accept natural completion or stop fallback.
# 6. Assert DAC/AMP muted after playback ends.
def test_playback_unmutes_has_expected_duration_and_blocks_test_modes(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    filename = f"{_unique_name('playback-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    t_start = time.monotonic()
    status, _, body = _audio_playback_start(base_url, filename)
    assert status == 200, f"Failed to start playback. body={body}"

    playing_state: dict[str, object] = {}
    def _playback_for_target_file_is_active() -> bool:
        nonlocal playing_state
        playing_state = _audio_playback_state(base_url)
        return bool(playing_state.get("active")) and playing_state.get("file") == filename

    playing = _wait_until(
        _playback_for_target_file_is_active,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert playing, f"Playback did not become active.\nLog tail:\n{_tail_log(log_path)}"
    assert playing_state.get("active") is True, playing_state
    assert playing_state.get("file") == filename, playing_state
    assert playing_state.get("skipped") in ("", None), playing_state

    unmuted = _wait_until(lambda: _outputs_are_unmuted(base_url), timeout_s=3.0, poll_s=0.1)
    assert unmuted, f"DAC/AMP did not unmute during playback.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _audio_test_post(base_url, action="start", freq=440.0, volume=25.0)
    assert status == 409, f"Expected tone blocked during playback. body={body}"
    assert "Audio playback in progress" in body

    status, _, body = _audio_test_post(base_url, action="sweep")
    assert status == 409, f"Expected sweep blocked during playback. body={body}"
    assert "Audio playback in progress" in body

    elapsed_min_ok = _wait_until(
        lambda: (time.monotonic() - t_start) >= 5.0,
        timeout_s=10.0,
        poll_s=0.1,
    )
    assert elapsed_min_ok, "Playback did not stay active for a meaningful duration."

    playback_done = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=12.0,
        poll_s=0.1,
    )
    if not playback_done:
        # QEMU audio sinks can stall draining; stop explicitly so mute/unblock behavior is still verified.
        status, _, body = _audio_playback_stop(base_url, timeout_s=8.0)
        assert status == 200, f"Failed to stop playback fallback. body={body}"
        playback_done = _wait_until(
            lambda: not bool(_audio_playback_state(base_url).get("active")),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert playback_done, f"Playback did not clear after stop fallback.\nLog tail:\n{_tail_log(log_path)}"

    elapsed_s = time.monotonic() - t_start
    assert elapsed_s >= 5.0, f"Playback ended too quickly: {elapsed_s:.2f}s"

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, f"DAC/AMP did not return to muted after playback.\nLog tail:\n{_tail_log(log_path)}"


# Test: `GET /audio/playback` reports a skipped file when track volume is 0%.
# 1. Start from main app mode and wait for any startup sound playback to finish.
# 2. Upload the `test6165ms.mp3` fixture and set its track volume to `0`.
# 3. Start playback and assert the request is accepted but playback never becomes active.
# 4. Poll `GET /audio/playback` and assert it reports `active=false`, the target `file`, and the skip reason.
# 5. Assert logs record the skip reason and no real playback start is attempted.
def test_manual_playback_with_zero_track_volume_is_skipped_and_logged(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert idle, f"Startup playback did not clear before zero-volume test.\nLog tail:\n{_tail_log(log_path)}"

    filename = f"{_unique_name('zero-volume-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _set_sound_meta(base_url, filename, {"volume": 0})
    assert status == 200, f"Failed to set track volume to 0. body={body}"
    assert body == "OK"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    status, _, body = _audio_playback_start(base_url, filename)
    assert status == 200, f"Failed to start zero-volume playback. body={body}"

    playback_state: dict[str, object] = {}
    state_reported = _wait_until(
        lambda: (
            (playback_state := _audio_playback_state(base_url))
            and playback_state.get("active") is False
            and playback_state.get("file") == filename
            and playback_state.get("skipped") == "track volume is 0%"
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert state_reported, (
        "Expected GET /audio/playback to report the skipped zero-volume file.\n"
        f"Last state: {playback_state}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    skip_logged = _wait_until(
        lambda: _log_since_contains_all(
            log_path,
            log_start_pos,
            ["Skipping playback:", filename, "track volume is 0%"],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert skip_logged, (
        "Expected logs to show the zero-volume track playback was skipped.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
    assert stayed_inactive, (
        "Zero-volume playback unexpectedly became active instead of being skipped.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    assert not _log_contains_any_since(
        log_path,
        log_start_pos,
        [f"Starting playback: {filename}"],
    ), f"Zero-volume track should not start real playback.\nLog tail:\n{_tail_log(log_path)}"
    assert _outputs_are_muted(base_url), (
        f"DAC/AMP should stay muted when skipping zero-volume track playback.\nLog tail:\n{_tail_log(log_path)}"
    )


@pytest.mark.parametrize(
    ("case_name", "hall_level", "audio_config", "skip_reason"),
    [
        ("lid-closed", 0, {"lid_closed_volume_pct": 0, "lid_open_volume_pct": 100}, "lid-closed volume is 0%"),
        ("lid-open", 1, {"lid_closed_volume_pct": 100, "lid_open_volume_pct": 0}, "lid-open volume is 0%"),
    ],
)
def test_playback_with_zero_active_lid_volume_is_skipped_and_logged(
    qemu_mainapp_instance,
    case_name: str,
    hall_level: int,
    audio_config: dict,
    skip_reason: str,
):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert idle, (
        "Startup playback did not clear before zero active-lid-volume test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    audio_cfg = _set_audio_config(base_url, audio_config)
    assert audio_cfg.get("lid_closed_volume_pct") == audio_config["lid_closed_volume_pct"]
    assert audio_cfg.get("lid_open_volume_pct") == audio_config["lid_open_volume_pct"]

    _set_test_gpio_level(base_url, "hall", hall_level)

    filename = f"{_unique_name(f'{case_name}-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _set_sound_meta(base_url, filename, {"volume": 100})
    assert status == 200, f"Failed to set track volume to 100. body={body}"
    assert body == "OK"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    status, _, body = _audio_playback_start(base_url, filename)
    assert status == 200, f"Failed to start playback for {case_name}. body={body}"

    playback_state: dict[str, object] = {}
    state_reported = _wait_until(
        lambda: (
            (playback_state := _audio_playback_state(base_url))
            and playback_state.get("active") is False
            and playback_state.get("file") == filename
            and playback_state.get("skipped") == skip_reason
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert state_reported, (
        f"Expected GET /audio/playback to report the skipped {case_name} file.\n"
        f"Last state: {playback_state}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    skip_logged = _wait_until(
        lambda: _log_since_contains_all(
            log_path,
            log_start_pos,
            ["Skipping playback:", filename, skip_reason],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert skip_logged, (
        f"Expected logs to show {case_name} playback was skipped because the active lid volume is 0%.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
    assert stayed_inactive, (
        f"{case_name} playback unexpectedly became active instead of being skipped.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    assert not _log_contains_any_since(
        log_path,
        log_start_pos,
        [f"Starting playback: {filename}"],
    ), f"{case_name} zero active-lid-volume playback should not start real playback.\nLog tail:\n{_tail_log(log_path)}"
    assert _outputs_are_muted(base_url), (
        f"DAC/AMP should stay muted when skipping {case_name} zero active-lid-volume playback.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: Laser-triggered playback of a 0%-volume sound is skipped immediately.
# 1. Start from main app mode and wait for any startup sound playback to finish.
# 2. Upload the `test6165ms.mp3` fixture and make it the only weighted laser candidate with volume `0`.
# 3. Trigger laser playback and assert the laser path still selects the file deterministically.
# 4. Assert logs record that playback is skipped and no real playback start is attempted.
# 5. Assert playback never becomes active over the early skip window.
def test_laser_playback_with_zero_track_volume_is_skipped_and_logged(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert idle, (
        "Startup playback did not clear before zero-volume laser playback test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('laser-zero-volume-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _set_sound_meta(base_url, DEFAULT_SOUND_FILENAME, {"probability": 0, "enabled": True})
    assert status == 200, f"Failed to remove default sound from laser selection. body={body}"
    assert body == "OK"

    status, _, body = _set_sound_meta(
        base_url,
        filename,
        {"enabled": True, "probability": 100, "volume": 0},
    )
    assert status == 200, f"Failed to configure zero-volume laser candidate. body={body}"
    assert body == "OK"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _trigger_laser_playback(base_url)

    selected = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Coin detected! Starting playback of {filename} (candidates=1, total_weight=100)"],
        ),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert selected, (
        "Laser path did not deterministically select the zero-volume file before skip handling.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    playback_state: dict[str, object] = {}
    state_reported = _wait_until(
        lambda: (
            (playback_state := _audio_playback_state(base_url))
            and playback_state.get("active") is False
            and playback_state.get("file") == filename
            and playback_state.get("skipped") == "track volume is 0%"
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert state_reported, (
        "Expected GET /audio/playback to report the skipped laser-selected file.\n"
        f"Last state: {playback_state}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    skip_logged = _wait_until(
        lambda: _log_since_contains_all(
            log_path,
            log_start_pos,
            ["Skipping playback:", filename, "track volume is 0%"],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert skip_logged, (
        "Expected logs to show the laser-triggered zero-volume playback was skipped.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
    assert stayed_inactive, (
        "Laser-triggered zero-volume playback unexpectedly became active.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    assert not _log_contains_any_since(
        log_path,
        log_start_pos,
        [f"Starting playback: {filename}"],
    ), f"Laser-triggered zero-volume file should not start real playback.\nLog tail:\n{_tail_log(log_path)}"
    assert _outputs_are_muted(base_url), (
        f"DAC/AMP should stay muted when laser-triggered zero-volume playback is skipped.\nLog tail:\n{_tail_log(log_path)}"
    )


# Test: Rapid repeated playback-start requests keep the control plane responsive.
# 1. Upload real MP3 fixture.
# 2. Send a burst of playback-start requests for the same file.
# 3. Stop playback and assert the device still responds without panic markers.
def test_repeated_playback_restarts_keep_system_healthy(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    proc = qemu_mainapp_instance["process"]

    filename = f"{_unique_name('restart-spam-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    for _ in range(10):
        status, _, body = _audio_playback_start(base_url, filename, timeout_s=4.0)
        assert status == 200, f"Playback restart request failed. body={body}"
        time.sleep(0.02)

    status, _, body = _audio_playback_stop(base_url, timeout_s=4.0)
    assert status == 200, f"Playback stop failed after restart spam. body={body}"

    playback_done = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert playback_done, f"Playback did not clear after restart spam.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _http_get(base_url, "/sounds/")
    assert status == 200, f"/sounds/ is not healthy after playback restart spam. status={status}, body={body}"

    _assert_no_panic_since(log_path, log_start_pos)
    assert proc.poll() is None, "QEMU process exited during playback restart spam"
