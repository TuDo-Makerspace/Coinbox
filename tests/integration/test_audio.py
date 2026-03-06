from __future__ import annotations

import json
import time
from urllib.parse import urlencode

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


def _volume_pct(state: dict) -> float:
    return float(state.get("volume_pct", -1.0))


def _assert_no_panic_since(log_path, start_pos: int):
    has_panic = _log_contains_any_since(log_path, start_pos, PANIC_LOG_MARKERS)
    assert not has_panic, f"Detected panic markers after playback restart spam.\nLog tail:\n{_tail_log(log_path)}"


# Test: Device boots into main app with DAC + AMP muted.
# 1. Start from main app mode.
# 2. Poll test GPIO mute endpoints.
# 3. Assert DAC/AMP are in muted levels.
def test_boot_starts_with_dac_and_amp_muted(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, "Expected DAC+AMP to be muted after boot into main app."


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


# Test: Playback unmutes during play, lasts about file duration, and blocks test/sweep.
# 1. Upload real MP3 fixture (~6s).
# 2. Start playback and assert `playback_active=true`.
# 3. Assert DAC/AMP unmute during active playback.
# 4. Assert audio test start and sweep are rejected during playback.
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

    playing = _wait_until(
        lambda: bool(_audio_test_state(base_url).get("playback_active")),
        timeout_s=4.0,
        poll_s=0.1,
    )
    assert playing, f"Playback did not become active.\nLog tail:\n{_tail_log(log_path)}"

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
        lambda: not bool(_audio_test_state(base_url).get("playback_active")),
        timeout_s=12.0,
        poll_s=0.1,
    )
    if not playback_done:
        # QEMU audio sinks can stall draining; stop explicitly so mute/unblock behavior is still verified.
        status, _, body = _audio_playback_stop(base_url, timeout_s=8.0)
        assert status == 200, f"Failed to stop playback fallback. body={body}"
        playback_done = _wait_until(
            lambda: not bool(_audio_test_state(base_url).get("playback_active")),
            timeout_s=3.0,
            poll_s=0.1,
        )
        assert playback_done, f"Playback did not clear after stop fallback.\nLog tail:\n{_tail_log(log_path)}"

    elapsed_s = time.monotonic() - t_start
    assert elapsed_s >= 5.0, f"Playback ended too quickly: {elapsed_s:.2f}s"

    muted = _wait_until(lambda: _outputs_are_muted(base_url), timeout_s=3.0, poll_s=0.1)
    assert muted, f"DAC/AMP did not return to muted after playback.\nLog tail:\n{_tail_log(log_path)}"


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
        lambda: not bool(_audio_test_state(base_url).get("playback_active")),
        timeout_s=3.0,
        poll_s=0.1,
    )
    assert playback_done, f"Playback did not clear after restart spam.\nLog tail:\n{_tail_log(log_path)}"

    status, _, body = _http_get(base_url, "/sounds/")
    assert status == 200, f"/sounds/ is not healthy after playback restart spam. status={status}, body={body}"

    _assert_no_panic_since(log_path, log_start_pos)
    assert proc.poll() is None, "QEMU process exited during playback restart spam"
