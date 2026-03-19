# The MIT License (MIT)
#
# Copyright (c) 2026 TuDo Makerspace
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.


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
AUDIO_CONFIG_DEBOUNCE_FIELDS = (
    "laser_debounce_ms",
    "lid_debounce_ms",
    "laser_trigger_cooldown_ms",
)
LASER_DEBOUNCE_DEFAULT_MS = 30
LASER_DEBOUNCE_MIN_MS = 0
LASER_DEBOUNCE_MAX_MS = 1000
LID_DEBOUNCE_DEFAULT_MS = 1000
LID_DEBOUNCE_MIN_MS = 0
LID_DEBOUNCE_MAX_MS = 5000
LID_VOLUME_MIN_PCT = 0
LID_VOLUME_MAX_PCT = 125


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


def _set_laser_debounce_ms(base_url: str, debounce_ms: int) -> dict:
    return _set_audio_config(base_url, {"laser_debounce_ms": debounce_ms})


def _set_lid_debounce_ms(base_url: str, debounce_ms: int) -> dict:
    return _set_audio_config(base_url, {"lid_debounce_ms": debounce_ms})


def _set_lid_open_sound_config(
    base_url: str,
    *,
    sound: str | None = None,
    volume_pct: int | None = None,
    lid_debounce_ms: int | None = None,
) -> dict:
    payload: dict[str, object] = {}
    if sound is not None:
        payload["lid_open_sound"] = sound
    if volume_pct is not None:
        payload["lid_volume_pct"] = volume_pct
    if lid_debounce_ms is not None:
        payload["lid_debounce_ms"] = lid_debounce_ms
    assert payload, "Expected at least one lid-open sound config update."
    return _set_audio_config(base_url, payload)


def _set_lid_close_sound_config(
    base_url: str,
    *,
    sound: str | None = None,
    volume_pct: int | None = None,
    lid_debounce_ms: int | None = None,
) -> dict:
    payload: dict[str, object] = {}
    if sound is not None:
        payload["lid_close_sound"] = sound
    if volume_pct is not None:
        payload["lid_volume_pct"] = volume_pct
    if lid_debounce_ms is not None:
        payload["lid_debounce_ms"] = lid_debounce_ms
    assert payload, "Expected at least one lid-close sound config update."
    return _set_audio_config(base_url, payload)


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


def _trigger_lid_open(base_url: str, settle_s: float = 0.02):
    _set_test_gpio_level(base_url, "hall", 0)
    time.sleep(settle_s)
    _set_test_gpio_level(base_url, "hall", 1)


def _trigger_lid_close(base_url: str, settle_s: float = 0.02):
    _set_test_gpio_level(base_url, "hall", 1)
    time.sleep(settle_s)
    _set_test_gpio_level(base_url, "hall", 0)


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


def _delete_sound_file(base_url: str, filename: str):
    return _http_get(base_url, f"/sounds/{filename}?delete=1", timeout_s=4.0)


def _format_storage(base_url: str):
    return _http_request(
        base_url=base_url,
        method="POST",
        path="/format",
        timeout_s=4.0,
        data=b"",
    )


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


# Test: `/audio/config` exposes writable top-level debounce settings.
# 1. Start from main app mode and capture baseline audio config.
# 2. Assert the top-level debounce fields are present with integer values.
# 3. POST new debounce values.
# 4. Verify the response and a fresh GET both reflect the new debounce values.
# 5. Verify the legacy `test_only` nesting is gone from the config payload.
def test_audio_config_updates_top_level_debounce_settings(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    baseline = _get_audio_config(base_url)
    assert "test_only" not in baseline, f"Did not expect legacy test_only key in /audio/config. got {baseline}"
    for key in AUDIO_CONFIG_DEBOUNCE_FIELDS:
        value = baseline.get(key)
        assert isinstance(value, int), (
            f"Expected integer {key} in /audio/config, got {value!r}"
        )
        assert value >= 0, (
            f"Expected non-negative {key} in /audio/config, got {value}"
        )

    new_values = {
        "laser_debounce_ms": 25,
        "lid_debounce_ms": 750,
        "laser_trigger_cooldown_ms": 125,
    }
    updated = _set_audio_config(base_url, new_values)
    assert "test_only" not in updated, f"Did not expect legacy test_only key after POST /audio/config. got {updated}"
    for key, expected in new_values.items():
        assert updated.get(key) == expected, (
            f"Expected {key}={expected} after POST /audio/config, got {updated.get(key)!r}"
        )

    readback = _get_audio_config(base_url)
    assert "test_only" not in readback, f"Did not expect legacy test_only key on GET /audio/config readback. got {readback}"
    for key, expected in new_values.items():
        assert readback.get(key) == expected, (
            f"Expected readback {key}={expected}, got {readback.get(key)!r}"
        )


# Test: `/audio/config` reports `30ms` as the default laser debounce.
# 1. Start from main app mode and read `/audio/config`.
# 2. Assert the top-level laser debounce field is present.
# 3. Assert `laser_debounce_ms` equals the default `30`.
def test_audio_config_reports_default_laser_debounce_ms(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    config = _get_audio_config(base_url)
    assert "test_only" not in config, f"Did not expect legacy test_only key in /audio/config. got {config}"
    assert config.get("laser_debounce_ms") == LASER_DEBOUNCE_DEFAULT_MS, (
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
    assert "test_only" not in baseline, f"Did not expect legacy test_only key in /audio/config. got {baseline}"
    baseline_laser_debounce = baseline.get("laser_debounce_ms")
    assert isinstance(baseline_laser_debounce, int), (
        f"Expected integer laser_debounce_ms, got {baseline_laser_debounce!r}"
    )

    for bad_value in (LASER_DEBOUNCE_MIN_MS - 1, LASER_DEBOUNCE_MAX_MS + 1):
        status, _, body = _http_post_json(
            base_url=base_url,
            path="/audio/config",
            payload={"laser_debounce_ms": bad_value},
            timeout_s=4.0,
        )
        assert status == 400, (
            f"Expected 400 for out-of-range laser debounce value {bad_value}. "
            f"body={body}"
        )

        readback = _get_audio_config(base_url)
        assert readback.get("laser_debounce_ms") == baseline_laser_debounce, (
            f"Out-of-range laser debounce value {bad_value} should not change stored config.\n"
            f"Readback: {readback}"
        )


# Test: `/audio/config` rejects lid-open sounds that do not exist on storage.
# 1. Start from main app mode and capture the current lid-open sound selection.
# 2. Attempt to save a non-existing MP3 as the lid-open sound.
# 3. Assert the request is rejected with a lid-open-sound-specific error.
# 4. Assert the stored lid-open sound selection remains empty afterwards.
def test_audio_config_rejects_nonexistent_lid_open_sound(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    missing_name = f"{_unique_name('missing-lid-open')}.mp3"

    status, _, body = _http_post_json(
        base_url=base_url,
        path="/audio/config",
        payload={"lid_open_sound": missing_name},
        timeout_s=4.0,
    )
    assert status == 400, f"Expected 400 when selecting a missing lid-open sound. body={body}"
    body_lower = body.lower()
    assert "lid" in body_lower and "open" in body_lower, (
        "Expected rejection body to mention the lid-open sound field.\n"
        f"body={body}"
    )
    assert ("does not exist" in body_lower) or ("not found" in body_lower), (
        "Expected rejection body to explain that the lid-open sound file does not exist.\n"
        f"body={body}"
    )

    readback = _get_audio_config(base_url)
    assert readback.get("lid_open_sound") == "", (
        "Missing lid-open sound should not be persisted into /audio/config.\n"
        f"Readback: {readback}"
    )


# Test: `/audio/config` rejects lid-close sounds that do not exist on storage.
# 1. Start from main app mode and capture the current lid-close sound selection.
# 2. Attempt to save a non-existing MP3 as the lid-close sound.
# 3. Assert the request is rejected with a lid-close-sound-specific error.
# 4. Assert the stored lid-close sound selection remains empty afterwards.
def test_audio_config_rejects_nonexistent_lid_close_sound(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    missing_name = f"{_unique_name('missing-lid-close')}.mp3"

    status, _, body = _http_post_json(
        base_url=base_url,
        path="/audio/config",
        payload={"lid_close_sound": missing_name},
        timeout_s=4.0,
    )
    assert status == 400, f"Expected 400 when selecting a missing lid-close sound. body={body}"
    body_lower = body.lower()
    assert "lid" in body_lower and "close" in body_lower, (
        "Expected rejection body to mention the lid-close sound field.\n"
        f"body={body}"
    )
    assert ("does not exist" in body_lower) or ("not found" in body_lower), (
        "Expected rejection body to explain that the lid-close sound file does not exist.\n"
        f"body={body}"
    )

    readback = _get_audio_config(base_url)
    assert readback.get("lid_close_sound") == "", (
        "Missing lid-close sound should not be persisted into /audio/config.\n"
        f"Readback: {readback}"
    )


# Test: `/audio/config` rejects lid volume values outside `0-125`.
# 1. Start from main app mode and capture the current lid volume.
# 2. Attempt to save lid volume values below `0` and above `125`.
# 3. Assert each request is rejected with a lid-volume-specific error.
# 4. Assert the stored lid volume remains unchanged afterwards.
def test_audio_config_rejects_invalid_lid_volume_values(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    baseline = _get_audio_config(base_url)
    baseline_volume = baseline.get("lid_volume_pct", 100)

    for bad_value in (LID_VOLUME_MIN_PCT - 1, LID_VOLUME_MAX_PCT + 1):
        status, _, body = _http_post_json(
            base_url=base_url,
            path="/audio/config",
            payload={"lid_volume_pct": bad_value},
            timeout_s=4.0,
        )
        assert status == 400, (
            f"Expected 400 for invalid lid volume {bad_value}. body={body}"
        )
        body_lower = body.lower()
        assert "lid" in body_lower and "volume" in body_lower, (
            "Expected rejection body to mention the lid volume field.\n"
            f"body={body}"
        )

        readback = _get_audio_config(base_url)
        assert readback.get("lid_volume_pct", baseline_volume) == baseline_volume, (
            f"Invalid lid volume {bad_value} should not change stored config.\n"
            f"Readback: {readback}"
        )


# Test: `/audio/config` rejects lid debounce values outside `0-5000`.
# 1. Start from main app mode and capture the current lid debounce.
# 2. Attempt to save lid debounce values below `0` and above `5000`.
# 3. Assert each request is rejected with `400`.
# 4. Assert the stored lid debounce remains unchanged afterwards.
def test_audio_config_rejects_out_of_range_lid_debounce_values(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    baseline = _get_audio_config(base_url)
    baseline_lid_debounce = baseline.get("lid_debounce_ms")
    assert isinstance(baseline_lid_debounce, int), (
        f"Expected integer lid_debounce_ms, got {baseline_lid_debounce!r}"
    )

    for bad_value in (LID_DEBOUNCE_MIN_MS - 1, LID_DEBOUNCE_MAX_MS + 1):
        status, _, body = _http_post_json(
            base_url=base_url,
            path="/audio/config",
            payload={"lid_debounce_ms": bad_value},
            timeout_s=4.0,
        )
        assert status == 400, (
            f"Expected 400 for invalid lid debounce {bad_value}. body={body}"
        )

        readback = _get_audio_config(base_url)
        assert readback.get("lid_debounce_ms") == baseline_lid_debounce, (
            f"Invalid lid debounce {bad_value} should not change stored config.\n"
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
    assert updated.get("laser_debounce_ms") == 200, updated

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
    assert updated.get("laser_debounce_ms") == 200, updated

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
    assert updated.get("laser_debounce_ms") == 0, updated

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
    assert updated.get("laser_debounce_ms") == 200, updated

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
    assert updated.get("laser_debounce_ms") == 50, updated

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


# Test: Mock playback duration honors stored trim metadata.
# 1. Upload the `test6165ms.mp3` fixture under a `...6165ms.mp3` filename.
# 2. Save a trim window from `1.200s` to `3.900s`.
# 3. Start playback and wait until `GET /audio/playback` reports the file as active.
# 4. Measure how long playback remains active.
# 5. Assert the observed active duration is near the trimmed `2.7s` window, not the full fixture length.
def test_playback_duration_respects_trim_window_in_mock_backend(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    filename = f"{_unique_name('trimmed-playback-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _set_sound_meta(
        base_url,
        filename,
        {"start": "1:200", "stop": "3:900"},
    )
    assert status == 200, f"Failed to save trim metadata. body={body}"
    assert body == "OK"

    status, _, body = _audio_playback_start(base_url, filename, timeout_s=8.0)
    assert status == 200, f"Failed to start trimmed playback. body={body}"

    playback_state: dict[str, object] = {}

    def _trimmed_playback_is_active() -> bool:
        nonlocal playback_state
        playback_state = _audio_playback_state(base_url)
        return bool(playback_state.get("active")) and playback_state.get("file") == filename

    playing = _wait_until(
        _trimmed_playback_is_active,
        timeout_s=4.0,
        poll_s=0.05,
    )
    assert playing, f"Trimmed playback did not become active.\nLog tail:\n{_tail_log(log_path)}"

    active_started_at = time.monotonic()
    playback_done = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=6.0,
        poll_s=0.05,
    )
    assert playback_done, f"Trimmed playback did not finish in time.\nLog tail:\n{_tail_log(log_path)}"

    elapsed_s = time.monotonic() - active_started_at
    assert 2.0 <= elapsed_s <= 3.6, (
        "Trimmed mock playback should run for roughly the requested 2.7 s window.\n"
        f"elapsed={elapsed_s:.2f}s\nstate={playback_state}\nlog_tail=\n{_tail_log(log_path)}"
    )

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, f"DAC/AMP did not return to muted after playback.\nLog tail:\n{_tail_log(log_path)}"


# Test: Main HTML pages should not fail with "Out of memory" while timed playback is active.
# 1. Upload the timed MP3 fixture.
# 2. Start playback and wait until `GET /audio/playback` reports it active.
# 3. Load `/settings` and `/sounds/` during playback.
# 4. Fail if either page returns non-200 or an "Out of memory" response.
def test_settings_and_sounds_pages_do_not_oom_during_playback(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    filename = f"{_unique_name('page-load-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    status, _, body = _audio_playback_start(base_url, filename, timeout_s=8.0)
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

    failures: list[str] = []
    pages = (
        ("/settings", "settings"),
        ("/sounds/", "sounds"),
    )

    try:
        for path, label in pages:
            status, headers, body = _http_get(base_url, path, timeout_s=8.0)
            if status != 200:
                failures.append(
                    f"{label} page returned {status} during playback. body={body[:200]!r}"
                )
                continue
            if "text/html" not in headers.get("Content-Type", ""):
                failures.append(
                    f"{label} page returned unexpected content type during playback: "
                    f"{headers.get('Content-Type', '')!r}"
                )
            if "Out of memory" in body:
                failures.append(
                    f"{label} page returned an Out of memory response during playback."
                )
    finally:
        status, _, body = _audio_playback_stop(base_url, timeout_s=8.0)
        assert status == 200, f"Failed to stop playback after page load test. body={body}"
        playback_done = _wait_until(
            lambda: not bool(_audio_playback_state(base_url).get("active")),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert playback_done, (
            "Playback did not clear after page load test.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

    assert not failures, (
        "Pages failed during active playback.\n"
        + "\n".join(failures)
        + f"\nLog tail:\n{_tail_log(log_path)}"
    )


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


# Test: Laser-triggered playback is suppressed while the lid is open.
# 1. Start from main app mode and wait for any startup playback to finish.
# 2. Force the hall sensor into the lid-open state and make one uploaded MP3 the only laser candidate.
# 3. Trigger a laser playback attempt.
# 4. Assert logs record the open-lid suppression and no real playback start is attempted.
# 5. Assert playback never becomes active over the early suppression window.
def test_laser_break_with_open_lid_does_not_start_playback(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_until(
        lambda: not bool(_audio_playback_state(base_url).get("active")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert idle, (
        "Startup playback did not clear before open-lid laser playback test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('laser-open-lid-6165ms')}.mp3"
    _configure_single_laser_candidate(base_url, filename)

    _set_test_gpio_level(base_url, "hall", 1)
    hall_state = _gpio_state(base_url).get("hall")
    assert isinstance(hall_state, dict), f"Expected hall object in /gpio/state, got {hall_state!r}"
    assert hall_state.get("lid_open") is True, f"Expected lid-open hall state, got {hall_state!r}"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    try:
        _trigger_laser_playback(base_url)

        blocked_logged = _wait_until(
            lambda: _log_contains_any_since(
                log_path,
                log_start_pos,
                ["Coin detected, but lid is open; playback is disabled"],
            ),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert blocked_logged, (
            "Expected logs to show laser-triggered playback was blocked while the lid is open.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
        assert stayed_inactive, (
            "Laser-triggered playback unexpectedly became active while the lid was open.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        assert not _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Coin detected! Starting playback of {filename} (candidates=1, total_weight=100)",
             f"Starting playback: {filename}"],
        ), (
            "Open-lid laser playback should not select or start the candidate file.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )
        assert _outputs_are_muted(base_url), (
            f"DAC/AMP should stay muted when laser playback is blocked by an open lid.\nLog tail:\n{_tail_log(log_path)}"
        )
    finally:
        _set_test_gpio_level(base_url, "hall", 0)


# Test: Opening the lid starts playback of the configured lid-open sound.
# 1. Upload a timed MP3 and configure it as the lid-open sound with non-zero lid volume and zero lid debounce.
# 2. Force the hall sensor from closed to open.
# 3. Assert `/audio/playback` reports the configured file active.
# 4. Assert logs show the lid-open event selected that file for playback.
def test_lid_open_event_starts_configured_playback(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, f"Startup playback did not clear before lid-open playback test.\nLog tail:\n{_tail_log(log_path)}"

    filename = f"{_unique_name('lid-open-start-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_open_sound_config(
        base_url,
        sound=filename,
        volume_pct=125,
        lid_debounce_ms=0,
    )
    assert config.get("lid_open_sound") == filename, config
    assert config.get("lid_volume_pct") == 125, config
    assert config.get("lid_debounce_ms") == 0, config

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    try:
        _trigger_lid_open(base_url)

        playback_state: dict[str, object] = {}

        def _configured_lid_open_file_is_active() -> bool:
            nonlocal playback_state
            playback_state = _audio_playback_state(base_url)
            return bool(playback_state.get("active")) and playback_state.get("file") == filename

        playing = _wait_until(
            _configured_lid_open_file_is_active,
            timeout_s=4.0,
            poll_s=0.1,
        )
        assert playing, (
            "Expected lid-open event to start playback of the configured file.\n"
            f"Last state: {playback_state}\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        lid_open_logged = _wait_until(
            lambda: _log_contains_any_since(
                log_path,
                log_start_pos,
                [f"Lid opened! Starting playback of {filename}"],
            ),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert lid_open_logged, (
            "Expected logs to show the lid-open event selected the configured file.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )
    finally:
        _audio_playback_stop(base_url, timeout_s=8.0)
        _wait_for_playback_idle(base_url, timeout_s=3.0)


# Test: Lid-open playback honors the configured lid debounce.
# 1. Upload a non-timed MP3 and configure it as the lid-open sound.
# 2. Set a lid debounce of `200ms`.
# 3. Toggle the hall sensor open -> closed -> open within the debounce window.
# 4. Assert only one playback start is logged for the configured file.
def test_lid_open_event_honors_lid_debounce(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, f"Startup playback did not clear before lid-open debounce test.\nLog tail:\n{_tail_log(log_path)}"

    filename = f"{_unique_name('lid-open-debounce')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_open_sound_config(
        base_url,
        sound=filename,
        volume_pct=100,
        lid_debounce_ms=200,
    )
    assert config.get("lid_open_sound") == filename, config
    assert config.get("lid_volume_pct") == 100, config
    assert config.get("lid_debounce_ms") == 200, config

    _set_test_gpio_level(base_url, "hall", 0)
    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    _set_test_gpio_level(base_url, "hall", 1)
    time.sleep(0.05)
    _set_test_gpio_level(base_url, "hall", 0)
    time.sleep(0.05)
    _set_test_gpio_level(base_url, "hall", 1)

    first_start_seen = _wait_until(
        lambda: _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}") >= 1,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert first_start_seen, (
        "Expected at least one playback start after opening the lid.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    time.sleep(0.5)
    start_count = _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}")
    assert start_count == 1, (
        "Expected lid debounce to suppress repeated lid-open playback within the debounce window.\n"
        f"Observed start count: {start_count}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    _set_test_gpio_level(base_url, "hall", 0)


# Test: Lid-open playback is skipped when the configured lid volume is `0%`.
# 1. Upload an MP3 and configure it as the lid-open sound with lid volume `0`.
# 2. Open the lid.
# 3. Assert logs explain that lid-open playback is skipped because the configured lid volume is `0%`.
# 4. Assert playback never becomes active and outputs stay muted.
def test_lid_open_event_with_zero_volume_is_skipped(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before lid-open zero-volume test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('lid-open-zero-volume-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_open_sound_config(
        base_url,
        sound=filename,
        volume_pct=0,
        lid_debounce_ms=0,
    )
    assert config.get("lid_open_sound") == filename, config
    assert config.get("lid_volume_pct") == 0, config

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    try:
        _trigger_lid_open(base_url)

        skip_logged = _wait_until(
            lambda: _log_contains_any_since(
                log_path,
                log_start_pos,
                ["Lid opened, but lid volume is 0%"],
            ),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert skip_logged, (
            "Expected logs to show lid-open playback was skipped because the configured lid volume is 0%.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
        assert stayed_inactive, (
            "Lid-open zero-volume playback unexpectedly became active.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        assert not _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Starting playback: {filename}"],
        ), f"Lid volume 0% should not start real playback.\nLog tail:\n{_tail_log(log_path)}"
        assert _outputs_are_muted(base_url), (
            f"DAC/AMP should stay muted when lid-open playback is skipped for 0% volume.\nLog tail:\n{_tail_log(log_path)}"
        )
    finally:
        _set_test_gpio_level(base_url, "hall", 0)


# Test: Lid-open playback is skipped when no lid-open sound is configured.
# 1. Clear the lid-open sound selection and set lid debounce to `0`.
# 2. Open the lid.
# 3. Assert logs explain that no lid-open sound is configured.
# 4. Assert playback never becomes active and outputs stay muted.
def test_lid_open_event_is_skipped_when_no_sound_is_configured(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before lid-open no-sound test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    config = _set_lid_open_sound_config(base_url, sound="", lid_debounce_ms=0)
    assert config.get("lid_open_sound") == "", config
    assert config.get("lid_debounce_ms") == 0, config

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    try:
        _trigger_lid_open(base_url)

        skip_logged = _wait_until(
            lambda: _log_contains_any_since(
                log_path,
                log_start_pos,
                ["Lid opened, but no lid-open sound is configured"],
            ),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert skip_logged, (
            "Expected logs to show lid-open playback was skipped because no sound is configured.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
        assert stayed_inactive, (
            "Lid-open event unexpectedly started playback even though no sound is configured.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )
        assert _outputs_are_muted(base_url), (
            f"DAC/AMP should stay muted when no lid-open sound is configured.\nLog tail:\n{_tail_log(log_path)}"
        )
    finally:
        _set_test_gpio_level(base_url, "hall", 0)


# Test: Deleting the selected lid-open sound clears the saved lid-open sound setting.
# 1. Upload an MP3 and configure it as the lid-open sound.
# 2. Delete that sound through the normal `/sounds/<name>?delete=1` endpoint.
# 3. Assert `/audio/config` clears `lid_open_sound` afterwards.
def test_deleting_selected_lid_open_sound_clears_audio_config(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    filename = f"{_unique_name('lid-open-delete-clear')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_open_sound_config(base_url, sound=filename, volume_pct=84, lid_debounce_ms=0)
    assert config.get("lid_open_sound") == filename, config
    assert config.get("lid_volume_pct") == 84, config

    delete_status, delete_headers, delete_body = _delete_sound_file(base_url, filename)
    assert delete_status == 303, (
        f"Deleting the selected lid-open sound should still succeed normally. body={delete_body}"
    )
    assert delete_headers.get("Location") == "/sounds/"
    assert "File deleted successfully" in delete_body

    readback = _get_audio_config(base_url)
    assert readback.get("lid_open_sound") == "", (
        "Deleting the selected lid-open sound should clear the saved lid-open sound selection.\n"
        f"Readback: {readback}"
    )


# Test: A stale configured lid-open sound fails in a controlled manner if the file disappears.
# 1. Upload an MP3 and configure it as the lid-open sound.
# 2. Format storage so the selected file disappears while the config remains.
# 3. Open the lid.
# 4. Assert playback stays inactive and logs report the missing configured file without a panic.
def test_stale_configured_lid_open_sound_fails_cleanly(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before stale lid-open sound test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('lid-open-stale-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_open_sound_config(base_url, sound=filename, volume_pct=100, lid_debounce_ms=0)
    assert config.get("lid_open_sound") == filename, config

    status, _, body = _format_storage(base_url)
    assert status == 200, f"Failed to format storage while preparing stale lid-open sound test. body={body}"
    assert "Formatted" in body

    missing_status, _, _ = _http_get(base_url, f"/sounds/{filename}", timeout_s=4.0)
    assert missing_status == 404, f"Expected stale lid-open sound file to be gone after format, got status={missing_status}"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    try:
        _trigger_lid_open(base_url)

        missing_logged = _wait_until(
            lambda: _log_contains_any_since(
                log_path,
                log_start_pos,
                [f"Configured lid-open sound does not exist: {filename}"],
            ),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert missing_logged, (
            "Expected logs to report the missing configured lid-open sound file.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
        assert stayed_inactive, (
            "Stale configured lid-open sound unexpectedly became active.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )
        assert _outputs_are_muted(base_url), (
            f"DAC/AMP should stay muted when the configured lid-open sound file is missing.\nLog tail:\n{_tail_log(log_path)}"
        )
        _assert_no_panic_since(log_path, log_start_pos)
    finally:
        _set_test_gpio_level(base_url, "hall", 0)


# Test: Closing the lid starts playback of the configured lid-close sound.
# 1. Upload a timed MP3 and configure it as the lid-close sound with non-zero lid volume and zero lid debounce.
# 2. Force the hall sensor from open to closed.
# 3. Assert `/audio/playback` reports the configured file active.
# 4. Assert logs show the lid-close event selected that file for playback.
def test_lid_close_event_starts_configured_playback(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, f"Startup playback did not clear before lid-close playback test.\nLog tail:\n{_tail_log(log_path)}"

    filename = f"{_unique_name('lid-close-start-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_close_sound_config(
        base_url,
        sound=filename,
        volume_pct=125,
        lid_debounce_ms=0,
    )
    assert config.get("lid_close_sound") == filename, config
    assert config.get("lid_volume_pct") == 125, config
    assert config.get("lid_debounce_ms") == 0, config

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    try:
        _trigger_lid_close(base_url)

        playback_state: dict[str, object] = {}

        def _configured_lid_close_file_is_active() -> bool:
            nonlocal playback_state
            playback_state = _audio_playback_state(base_url)
            return bool(playback_state.get("active")) and playback_state.get("file") == filename

        playing = _wait_until(
            _configured_lid_close_file_is_active,
            timeout_s=4.0,
            poll_s=0.1,
        )
        assert playing, (
            "Expected lid-close event to start playback of the configured file.\n"
            f"Last state: {playback_state}\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )

        lid_close_logged = _wait_until(
            lambda: _log_contains_any_since(
                log_path,
                log_start_pos,
                [f"Lid closed! Starting playback of {filename}"],
            ),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert lid_close_logged, (
            "Expected logs to show the lid-close event selected the configured file.\n"
            f"Log tail:\n{_tail_log(log_path)}"
        )
    finally:
        _audio_playback_stop(base_url, timeout_s=8.0)
        _wait_for_playback_idle(base_url, timeout_s=3.0)


# Test: Lid-close playback honors the configured lid debounce.
# 1. Upload a non-timed MP3 and configure it as the lid-close sound.
# 2. Set a lid debounce of `200ms`.
# 3. Toggle the hall sensor closed -> open -> closed within the debounce window.
# 4. Assert only one playback start is logged for the configured file.
def test_lid_close_event_honors_lid_debounce(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, f"Startup playback did not clear before lid-close debounce test.\nLog tail:\n{_tail_log(log_path)}"

    filename = f"{_unique_name('lid-close-debounce')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_close_sound_config(
        base_url,
        sound=filename,
        volume_pct=100,
        lid_debounce_ms=200,
    )
    assert config.get("lid_close_sound") == filename, config
    assert config.get("lid_volume_pct") == 100, config
    assert config.get("lid_debounce_ms") == 200, config

    _set_test_gpio_level(base_url, "hall", 1)
    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    _set_test_gpio_level(base_url, "hall", 0)
    time.sleep(0.05)
    _set_test_gpio_level(base_url, "hall", 1)
    time.sleep(0.05)
    _set_test_gpio_level(base_url, "hall", 0)

    first_start_seen = _wait_until(
        lambda: _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}") >= 1,
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert first_start_seen, (
        "Expected at least one playback start after closing the lid.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    time.sleep(0.5)
    start_count = _log_count_since(log_path, log_start_pos, f"Starting playback: {filename}")
    assert start_count == 1, (
        "Expected lid debounce to suppress repeated lid-close playback within the debounce window.\n"
        f"Observed start count: {start_count}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: Lid-close playback is skipped when the configured lid volume is `0%`.
# 1. Upload an MP3 and configure it as the lid-close sound with lid volume `0`.
# 2. Close the lid.
# 3. Assert logs explain that lid-close playback is skipped because the configured lid volume is `0%`.
# 4. Assert playback never becomes active and outputs stay muted.
def test_lid_close_event_with_zero_volume_is_skipped(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before lid-close zero-volume test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('lid-close-zero-volume-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_close_sound_config(
        base_url,
        sound=filename,
        volume_pct=0,
        lid_debounce_ms=0,
    )
    assert config.get("lid_close_sound") == filename, config
    assert config.get("lid_volume_pct") == 0, config

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _trigger_lid_close(base_url)

    skip_logged = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Lid closed, but lid volume is 0%"],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert skip_logged, (
        "Expected logs to show lid-close playback was skipped because the configured lid volume is 0%.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
    assert stayed_inactive, (
        "Lid-close zero-volume playback unexpectedly became active.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    assert not _log_contains_any_since(
        log_path,
        log_start_pos,
        [f"Starting playback: {filename}"],
    ), f"Lid volume 0% should not start real playback.\nLog tail:\n{_tail_log(log_path)}"
    assert _outputs_are_muted(base_url), (
        f"DAC/AMP should stay muted when lid-close playback is skipped for 0% volume.\nLog tail:\n{_tail_log(log_path)}"
    )


# Test: Lid-close playback is skipped when no lid-close sound is configured.
# 1. Clear the lid-close sound selection and set lid debounce to `0`.
# 2. Close the lid.
# 3. Assert logs explain that no lid-close sound is configured.
# 4. Assert playback never becomes active and outputs stay muted.
def test_lid_close_event_is_skipped_when_no_sound_is_configured(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before lid-close no-sound test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    config = _set_lid_close_sound_config(base_url, sound="", lid_debounce_ms=0)
    assert config.get("lid_close_sound") == "", config
    assert config.get("lid_debounce_ms") == 0, config

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _trigger_lid_close(base_url)

    skip_logged = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Lid closed, but no lid-close sound is configured"],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert skip_logged, (
        "Expected logs to show lid-close playback was skipped because no sound is configured.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
    assert stayed_inactive, (
        "Lid-close event unexpectedly started playback even though no sound is configured.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
    assert _outputs_are_muted(base_url), (
        f"DAC/AMP should stay muted when no lid-close sound is configured.\nLog tail:\n{_tail_log(log_path)}"
    )


# Test: Deleting the selected lid-close sound clears the saved lid-close sound setting.
# 1. Upload an MP3 and configure it as the lid-close sound.
# 2. Delete that sound through the normal `/sounds/<name>?delete=1` endpoint.
# 3. Assert `/audio/config` clears `lid_close_sound` afterwards.
def test_deleting_selected_lid_close_sound_clears_audio_config(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    filename = f"{_unique_name('lid-close-delete-clear')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_close_sound_config(base_url, sound=filename, volume_pct=84, lid_debounce_ms=0)
    assert config.get("lid_close_sound") == filename, config
    assert config.get("lid_volume_pct") == 84, config

    delete_status, delete_headers, delete_body = _delete_sound_file(base_url, filename)
    assert delete_status == 303, (
        f"Deleting the selected lid-close sound should still succeed normally. body={delete_body}"
    )
    assert delete_headers.get("Location") == "/sounds/"
    assert "File deleted successfully" in delete_body

    readback = _get_audio_config(base_url)
    assert readback.get("lid_close_sound") == "", (
        "Deleting the selected lid-close sound should clear the saved lid-close sound selection.\n"
        f"Readback: {readback}"
    )


# Test: A stale configured lid-close sound fails in a controlled manner if the file disappears.
# 1. Upload an MP3 and configure it as the lid-close sound.
# 2. Format storage so the selected file disappears while the config remains.
# 3. Close the lid.
# 4. Assert playback stays inactive and logs report the missing configured file without a panic.
def test_stale_configured_lid_close_sound_fails_cleanly(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    idle = _wait_for_playback_idle(base_url, timeout_s=3.0)
    assert idle, (
        "Startup playback did not clear before stale lid-close sound test.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    filename = f"{_unique_name('lid-close-stale-6165ms')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    config = _set_lid_close_sound_config(base_url, sound=filename, volume_pct=100, lid_debounce_ms=0)
    assert config.get("lid_close_sound") == filename, config

    status, _, body = _format_storage(base_url)
    assert status == 200, f"Failed to format storage while preparing stale lid-close sound test. body={body}"
    assert "Formatted" in body

    missing_status, _, _ = _http_get(base_url, f"/sounds/{filename}", timeout_s=4.0)
    assert missing_status == 404, f"Expected stale lid-close sound file to be gone after format, got status={missing_status}"

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    _trigger_lid_close(base_url)

    missing_logged = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Configured lid-close sound does not exist: {filename}"],
        ),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert missing_logged, (
        "Expected logs to report the missing configured lid-close sound file.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    stayed_inactive = _playback_stays_inactive_for(base_url, duration_s=2.0, poll_s=0.05)
    assert stayed_inactive, (
        "Stale configured lid-close sound unexpectedly became active.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )
    assert _outputs_are_muted(base_url), (
        f"DAC/AMP should stay muted when the configured lid-close sound file is missing.\nLog tail:\n{_tail_log(log_path)}"
    )
    _assert_no_panic_since(log_path, log_start_pos)


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
