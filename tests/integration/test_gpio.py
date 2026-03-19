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

import pytest

try:
    from tests.integration.integration_helpers import (
        _http_get,
        _http_request,
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
        _http_get,
        _http_request,
        _log_contains_any_since,
        _skip_to_main_app,
        _tail_log,
        _test_mp3_bytes,
        _upload_sound_file,
        _wait_until,
        qemu_bootstrap_instance,
    )


DEFAULT_SOUND_FILENAME = "default.mp3"
TEST_GPIO_WRITE_TIMEOUT_S = 6.0
TEST_GPIO_WRITE_RETRIES = 3
TEST_GPIO_RETRY_BACKOFF_S = 0.05


@pytest.fixture
def qemu_mainapp_instance(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    _skip_to_main_app(base_url, log_path)
    return qemu_bootstrap_instance


def _json_object(body: str, context: str) -> dict:
    try:
        value = json.loads(body)
    except json.JSONDecodeError as exc:
        pytest.fail(f"Failed to decode JSON from {context}: {exc}\nBody:\n{body}")
    assert isinstance(value, dict), f"Expected object JSON from {context}, got {type(value).__name__}"
    return value


def _unique_name(prefix: str) -> str:
    return f"{prefix}-{int(time.time() * 1000)}"


def _set_test_gpio_level(base_url: str, name: str, level: int):
    last_error: Exception | None = None
    last_status: int | None = None
    last_body = ""

    for attempt in range(TEST_GPIO_WRITE_RETRIES):
        try:
            status, _, body = _http_request(
                base_url=base_url,
                method="POST",
                path=f"/test/gpio/{name}?level={level}",
                timeout_s=TEST_GPIO_WRITE_TIMEOUT_S,
                data=b"",
            )
        except Exception as exc:
            last_error = exc
            if attempt + 1 < TEST_GPIO_WRITE_RETRIES:
                time.sleep(TEST_GPIO_RETRY_BACKOFF_S)
                continue
            raise

        last_status = status
        last_body = body
        if status == 200:
            payload = _json_object(body, f"POST /test/gpio/{name}")
            assert payload.get("level") == level
            return

        if attempt + 1 < TEST_GPIO_WRITE_RETRIES:
            time.sleep(TEST_GPIO_RETRY_BACKOFF_S)

    if last_error is not None:
        raise last_error
    assert last_status == 200, (
        f"Failed to set /test/gpio/{name}?level={level} after {TEST_GPIO_WRITE_RETRIES} attempts. "
        f"status={last_status}, body={last_body}"
    )


def _get_test_gpio_level(base_url: str, name: str) -> int:
    status, _, body = _http_get(base_url, f"/test/gpio/{name}")
    assert status == 200, f"Failed to get /test/gpio/{name}. status={status}, body={body}"
    payload = _json_object(body, f"GET /test/gpio/{name}")
    level = payload.get("level")
    assert level in (0, 1), f"Unexpected level for /test/gpio/{name}: {level}"
    return level


def _get_gpio_state(base_url: str) -> dict:
    status, _, body = _http_get(base_url, "/gpio/state")
    assert status == 200, f"Failed to get /gpio/state. status={status}, body={body}"
    return _json_object(body, "GET /gpio/state")


PANIC_LOG_MARKERS = [
    "Guru Meditation Error",
    "panic_abort",
    "Core  0 register dump",
    "Backtrace:",
]


def _assert_no_panic_since(log_path, start_pos: int):
    has_panic = _log_contains_any_since(log_path, start_pos, PANIC_LOG_MARKERS)
    assert not has_panic, f"Detected panic markers after GPIO spam.\nLog tail:\n{_tail_log(log_path)}"


def _assert_system_responsive(base_url: str):
    status, _, body = _http_get(base_url, "/runtime/status", timeout_s=4.0)
    assert status == 200, f"/runtime/status is not healthy after spam. status={status}, body={body}"

    status, _, body = _http_get(base_url, "/gpio/state", timeout_s=4.0)
    assert status == 200, f"/gpio/state is not healthy after spam. status={status}, body={body}"

    status, _, body = _http_get(base_url, "/logs", timeout_s=4.0)
    assert status == 200, f"/logs is not healthy after spam. status={status}, body={body}"


def _format_storage(base_url: str):
    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/format",
        timeout_s=10.0,
        data=b"",
    )
    assert status == 200, f"/format failed in main app. status={status}, body={body}"
    assert "Formatted" in body


# Test: Output mute endpoints reflect and update AMP/DAC mute GPIO levels.
# 1. Start from main app mode.
# 2. Set AMP mute via test endpoint and verify readback.
# 3. Set DAC mute via test endpoint and verify readback.
def test_output_gpio_endpoints_set_amp_and_dac_mute_levels(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    _set_test_gpio_level(base_url, "amp-mute", 0)
    assert _get_test_gpio_level(base_url, "amp-mute") == 0

    _set_test_gpio_level(base_url, "dac-mute", 1)
    assert _get_test_gpio_level(base_url, "dac-mute") == 1


# Test: Laser level maps to blocked/clear state in `/gpio/state`.
# 1. Start from main app mode.
# 2. Set test laser level to `1` and verify blocked=true.
# 3. Set test laser level to `0` and verify blocked=false.
def test_laser_level_reflects_blocked_and_clear_state(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    _set_test_gpio_level(base_url, "laser", 1)
    assert _get_test_gpio_level(base_url, "laser") == 1

    state = _get_gpio_state(base_url)
    laser = state.get("laser", {})
    assert laser.get("level") == 1
    assert laser.get("laser_blocked") is True

    _set_test_gpio_level(base_url, "laser", 0)
    assert _get_test_gpio_level(base_url, "laser") == 0

    state = _get_gpio_state(base_url)
    laser = state.get("laser", {})
    assert laser.get("level") == 0
    assert laser.get("laser_blocked") is False


# Test: Hall level maps to lid-closed/lid-open state in `/gpio/state`.
# 1. Start from main app mode.
# 2. Set test hall level to `0` and verify lid_open=false (closed).
# 3. Set test hall level to `1` and verify lid_open=true (open).
def test_hall_level_reflects_lid_closed_and_open_state(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    _set_test_gpio_level(base_url, "hall", 0)
    assert _get_test_gpio_level(base_url, "hall") == 0

    state = _get_gpio_state(base_url)
    hall = state.get("hall", {})
    assert hall.get("level") == 0
    assert hall.get("lid_open") is False

    _set_test_gpio_level(base_url, "hall", 1)
    assert _get_test_gpio_level(base_url, "hall") == 1

    state = _get_gpio_state(base_url)
    hall = state.get("hall", {})
    assert hall.get("level") == 1
    assert hall.get("lid_open") is True


# Test: Laser injection drives the laser task path and reaches audio playback path.
# 1. Start from main app mode and upload a real MP3 file.
# 2. Inject a laser clear->blocked transition.
# 3. Assert laser task marker appears in logs.
# 4. Assert audio playback-start marker appears in logs.
def test_laser_injection_reaches_audio_playback_path(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    filename = f"{_unique_name('laser-play')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    _set_test_gpio_level(base_url, "laser", 0)
    _set_test_gpio_level(base_url, "laser", 1)

    laser_task_seen = _wait_until(
        lambda: _log_contains_any_since(log_path, log_start_pos, ["Coin detected! Starting playback of"]),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert laser_task_seen, f"Laser task marker not found after injection.\nLog tail:\n{_tail_log(log_path)}"

    audio_playback_seen = _wait_until(
        lambda: _log_contains_any_since(log_path, log_start_pos, ["Starting playback:"]),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert audio_playback_seen, (
        "Audio playback marker not found after laser injection.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: With only the built-in default sound enabled, laser ISR playback picks `default.mp3`.
# 1. Start from main app mode and format storage to remove any previously uploaded sounds.
# 2. Inject a laser clear->blocked transition.
# 3. Assert logs show `default.mp3` selected with a single 100-weight candidate.
# 4. Assert the audio playback path starts for that file.
def test_laser_injection_plays_default_sound_when_it_is_only_enabled_file(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    _format_storage(base_url)
    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    _set_test_gpio_level(base_url, "laser", 0)
    _set_test_gpio_level(base_url, "laser", 1)

    selected_default = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            [f"Coin detected! Starting playback of {DEFAULT_SOUND_FILENAME} (candidates=1, total_weight=100)"],
        ),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert selected_default, (
        "Laser ISR did not select the built-in default sound as the only enabled candidate.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    audio_playback_seen = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Starting playback:", f"Playback started for file: {DEFAULT_SOUND_FILENAME}"],
        ),
        timeout_s=5.0,
        poll_s=0.2,
    )
    assert audio_playback_seen, (
        "Audio playback marker not found after default-sound laser injection.\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


# Test: Laser and hall graph events reflect injected GPIO changes.
# 1. Start from main app mode and confirm `/sounds/` is reachable.
# 2. Prime levels and drain any pre-existing events.
# 3. Inject a known sequence of laser and hall level changes.
# 4. Verify event arrays in `/gpio/state` contain the injected sequences.
def test_graphs_reflect_laser_and_hall_gpio_changes(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, _, _ = _http_get(base_url, "/sounds/")
    assert status == 200

    _set_test_gpio_level(base_url, "laser", 0)
    _set_test_gpio_level(base_url, "hall", 0)
    _get_gpio_state(base_url)

    laser_sequence = [1, 0, 1]
    hall_sequence = [1, 0, 1]

    for level in laser_sequence:
        _set_test_gpio_level(base_url, "laser", level)
    for level in hall_sequence:
        _set_test_gpio_level(base_url, "hall", level)

    gpio_state = _get_gpio_state(base_url)
    laser_state = gpio_state.get("laser", {})
    hall_state = gpio_state.get("hall", {})
    assert isinstance(laser_state, dict)
    assert isinstance(hall_state, dict)
    assert laser_state.get("level") == laser_sequence[-1]
    assert hall_state.get("level") == hall_sequence[-1]

    laser_events = [int(evt.get("level")) for evt in laser_state.get("events", [])]
    hall_events = [int(evt.get("level")) for evt in hall_state.get("events", [])]
    assert len(laser_events) >= len(laser_sequence), f"Laser events too short: {laser_events}"
    assert len(hall_events) >= len(hall_sequence), f"Hall events too short: {hall_events}"
    assert laser_events[-len(laser_sequence):] == laser_sequence
    assert hall_events[-len(hall_sequence):] == hall_sequence

# Test: Laser GPIO spam does not destabilize the system and still reaches playback path.
# 1. Start from main app mode and upload a real MP3.
# 2. Record initial laser change counter and log position.
# 3. Spam laser level writes with rapid 0/1 toggles.
# 4. Assert playback-related logs appear under spam.
# 5. Assert counter advanced by expected amount and system remains responsive.
# 6. Assert no panic markers were logged and QEMU process is still alive.
def test_laser_spam_keeps_system_healthy(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    proc = qemu_mainapp_instance["process"]

    filename = f"{_unique_name('laser-spam')}.mp3"
    _upload_sound_file(base_url, filename, _test_mp3_bytes())

    baseline = _get_gpio_state(base_url)
    baseline_changes = int(baseline["laser"]["changes"])
    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    toggle_count = 80
    last_level = 0
    for i in range(toggle_count):
        last_level = (i + 1) % 2
        _set_test_gpio_level(base_url, "laser", last_level)

    playback_path_seen = _wait_until(
        lambda: _log_contains_any_since(
            log_path,
            log_start_pos,
            ["Coin detected! Starting playback of", "Starting playback:"],
        ),
        timeout_s=6.0,
        poll_s=0.2,
    )
    assert playback_path_seen, f"Laser spam did not reach playback path.\nLog tail:\n{_tail_log(log_path)}"

    state = _get_gpio_state(base_url)
    laser = state["laser"]
    delta_changes = int(laser["changes"]) - baseline_changes
    expected_min_changes = toggle_count - 1
    assert delta_changes >= expected_min_changes, (
        f"Laser change counter did not advance as expected. "
        f"delta={delta_changes}, expected>={expected_min_changes}"
    )
    assert int(laser["level"]) == last_level
    assert bool(laser["laser_blocked"]) == bool(last_level)

    _assert_system_responsive(base_url)
    _assert_no_panic_since(log_path, log_start_pos)
    assert proc.poll() is None, "QEMU process exited during laser spam"


# Test: Hall GPIO spam does not destabilize the system and graph data remains valid.
# 1. Start from main app mode and record baseline hall change counter.
# 2. Spam hall level writes with rapid 0/1 toggles beyond event-buffer capacity.
# 3. Assert hall state tracks final level and lid-open flag correctly.
# 4. Assert hall event graph still reports events and dropped-count for overflow.
# 5. Assert system remains responsive and no panic markers are logged.
def test_hall_spam_keeps_system_healthy_and_graph_valid(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    proc = qemu_mainapp_instance["process"]

    baseline = _get_gpio_state(base_url)
    baseline_changes = int(baseline["hall"]["changes"])
    log_start_pos = log_path.stat().st_size if log_path.exists() else 0

    toggle_count = 180
    last_level = 0
    for i in range(toggle_count):
        last_level = (i + 1) % 2
        _set_test_gpio_level(base_url, "hall", last_level)

    state = _get_gpio_state(base_url)
    hall = state["hall"]
    delta_changes = int(hall["changes"]) - baseline_changes
    expected_min_changes = toggle_count - 1
    assert delta_changes >= expected_min_changes, (
        f"Hall change counter did not advance as expected. "
        f"delta={delta_changes}, expected>={expected_min_changes}"
    )
    assert int(hall["level"]) == last_level
    assert bool(hall["lid_open"]) == bool(last_level)

    hall_events = hall.get("events", [])
    assert isinstance(hall_events, list) and len(hall_events) > 0
    assert int(hall.get("events_dropped", 0)) >= 1, "Expected dropped hall events under buffer-pressure spam"

    _assert_system_responsive(base_url)
    _assert_no_panic_since(log_path, log_start_pos)
    assert proc.poll() is None, "QEMU process exited during hall spam"


# Test: Mixed GPIO spam (laser/hall/output mute) keeps control endpoints responsive.
# 1. Start from main app mode and record log position.
# 2. Spam mixed test GPIO endpoints in a tight loop.
# 3. Verify final endpoint readbacks match last written levels.
# 4. Assert system remains responsive and no panic markers are logged.
def test_mixed_gpio_spam_keeps_control_plane_healthy(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    proc = qemu_mainapp_instance["process"]

    log_start_pos = log_path.stat().st_size if log_path.exists() else 0
    expected = {
        "laser": 0,
        "hall": 0,
        "amp-mute": 0,
        "dac-mute": 0,
    }
    order = ["laser", "hall", "amp-mute", "dac-mute"]

    for i in range(160):
        name = order[i % len(order)]
        level = (i // len(order)) % 2
        expected[name] = level
        _set_test_gpio_level(base_url, name, level)

    for name, level in expected.items():
        assert _get_test_gpio_level(base_url, name) == level

    _assert_system_responsive(base_url)
    _assert_no_panic_since(log_path, log_start_pos)
    assert proc.poll() is None, "QEMU process exited during mixed GPIO spam"
