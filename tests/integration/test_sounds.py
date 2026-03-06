from __future__ import annotations

import json
import re
import time

import pytest

try:
    from tests.integration.integration_helpers import (
        _assert_sound_download_status,
        _http_get,
        _http_get_bytes,
        _http_request,
        _restart_into_bootstrap,
        _skip_to_main_app,
        _test_mp3_bytes,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        _assert_sound_download_status,
        _http_get,
        _http_get_bytes,
        _http_request,
        _restart_into_bootstrap,
        _skip_to_main_app,
        _test_mp3_bytes,
        qemu_bootstrap_instance,
    )


def _unique_name(prefix: str) -> str:
    return f"{prefix}-{int(time.time() * 1000)}"


def _upload_sound(base_url: str, filename: str, payload: bytes, timeout_s: float = 10.0):
    return _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/{filename}",
        timeout_s=timeout_s,
        data=payload,
        headers={"Content-Type": "audio/mpeg"},
    )


def _sound_meta_path(filename: str) -> str:
    return f"/sounds/file-meta/{filename}"


def _get_sound_meta(base_url: str, filename: str) -> dict:
    status, headers, body = _http_request(
        base_url=base_url,
        method="GET",
        path=_sound_meta_path(filename),
        timeout_s=4.0,
    )
    assert status == 200, f"Failed to fetch metadata for {filename}. status={status}, body={body}"
    assert "application/json" in headers.get("Content-Type", "")
    return json.loads(body)


def _set_sound_meta(base_url: str, filename: str, payload: dict):
    return _http_request(
        base_url=base_url,
        method="POST",
        path=_sound_meta_path(filename),
        timeout_s=4.0,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )


def _sized_mp3_payload(target_size: int) -> bytes:
    fixture = _test_mp3_bytes()
    repeats = (target_size // len(fixture)) + 1
    return (fixture * repeats)[:target_size]


def _extract_littlefs_mount_stats(log_path) -> tuple[int, int] | None:
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    matches = re.findall(r"LittleFS partition mounted at '.*' \(total: (\d+), used: (\d+)\)", text)
    if not matches:
        return None

    total_str, used_str = matches[-1]
    return int(total_str), int(used_str)


@pytest.fixture
def qemu_mainapp_instance(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    _skip_to_main_app(base_url, log_path)
    return qemu_bootstrap_instance


# Test: Root redirects to sounds index.
# 1. Start from main app mode.
# 2. Call `GET /`.
# 3. Assert redirect to `/sounds/`.
def test_root_redirects_to_sounds(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, headers, _ = _http_get(base_url, "/")
    assert status == 302
    assert headers.get("Location") == "/sounds/"


# Test: `/sounds` redirects to trailing-slash endpoint.
# 1. Start from main app mode.
# 2. Call `GET /sounds`.
# 3. Assert redirect to `/sounds/`.
def test_sounds_redirects_to_trailing_slash(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, headers, _ = _http_get(base_url, "/sounds")
    assert status == 302
    assert headers.get("Location") == "/sounds/"


# Test: `/skip` redirects to sounds index in main app mode.
# 1. Start from main app mode.
# 2. Call `GET /skip`.
# 3. Assert redirect to `/sounds/`.
def test_skip_redirects_to_sounds_in_main_app(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, headers, _ = _http_get(base_url, "/skip")
    assert status == 302
    assert headers.get("Location") == "/sounds/"


# Test: `/recovery` redirects to sounds index in main app mode.
# 1. Start from main app mode.
# 2. Call `GET /recovery`.
# 3. Assert redirect to `/sounds/`.
def test_recovery_redirects_to_sounds_in_main_app(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, headers, _ = _http_get(base_url, "/recovery")
    assert status == 302
    assert headers.get("Location") == "/sounds/"


# Test: Playback rejects non-existing file.
# 1. Start from main app mode.
# 2. Call `POST /audio/playback?name=<missing>.mp3`.
# 3. Assert `404` and not-found message.
def test_playback_rejects_nonexisting_file(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/audio/playback?name={_unique_name('missing')}.mp3",
        timeout_s=3.0,
        data=b"",
    )
    assert status == 404
    assert "File not found" in body


# Test: Upload accepts a valid MP3 file.
# 1. Start from main app mode.
# 2. Upload MP3 bytes to `POST /sounds/<name>.mp3`.
# 3. Assert redirect to `/sounds/`.
# 4. Verify the uploaded file can be downloaded.
def test_upload_real_mp3_file(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    filename = f"{_unique_name('upload-ok')}.mp3"
    status, headers, _ = _upload_sound(base_url, filename, _test_mp3_bytes())

    assert status == 303
    assert headers.get("Location") == "/sounds/"
    _assert_sound_download_status(base_url, filename, 200)


# Test: Downloaded MP3 exactly matches uploaded content.
# 1. Start from main app mode.
# 2. Upload real MP3 bytes to `POST /sounds/<name>.mp3`.
# 3. Download it via `GET /sounds/<name>.mp3`.
# 4. Assert byte-for-byte equality with the original payload.
def test_download_uploaded_mp3_matches_original(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('download-match')}.mp3"
    payload = _test_mp3_bytes()

    status, headers, body = _upload_sound(base_url, filename, payload)
    assert status == 303
    assert headers.get("Location") == "/sounds/"

    dl_status, dl_headers, dl_body = _http_get_bytes(base_url, f"/sounds/{filename}", timeout_s=8.0)
    assert dl_status == 200
    assert "audio/mpeg" in dl_headers.get("Content-Type", "")
    assert dl_body == payload


# Test: Upload rejects too-large filename.
# 1. Start from main app mode.
# 2. Upload with an excessively long `<name>.mp3`.
# 3. Assert filename-length rejection response.
def test_upload_rejects_too_large_filename(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    filename = ("n" * 300) + ".mp3"
    status, _, body = _upload_sound(base_url, filename, _test_mp3_bytes())

    assert status == 500
    assert "Filename too long" in body


# Test: Upload rejects invalid extension.
# 1. Start from main app mode.
# 2. Upload bytes to `POST /sounds/<name>.wav`.
# 3. Assert invalid-extension rejection.
# 4. Verify the rejected file was not created.
def test_upload_rejects_invalid_extension(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('bad-ext')}.wav"

    status, _, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/{filename}",
        timeout_s=3.0,
        data=_test_mp3_bytes(),
        headers={"Content-Type": "audio/wav"},
    )

    assert status == 400
    assert "Only audio files are allowed" in body
    _assert_sound_download_status(base_url, filename, 404)


# Test: Upload rejects oversized MP3 payload.
# 1. Start from main app mode.
# 2. Upload an MP3 payload larger than 2 MiB.
# 3. Assert `413` response with size-limit message.
# 4. Verify the rejected file was not created.
def test_upload_rejects_too_large_mp3(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('too-big')}.mp3"

    target_size = (2 * 1024 * 1024) + 1
    too_large_mp3 = _sized_mp3_payload(target_size)
    status, _, body = _upload_sound(base_url, filename, too_large_mp3)

    assert status == 413
    assert "File size must be less than 2MB" in body
    _assert_sound_download_status(base_url, filename, 404)


# Test: Uploading eventually reaches storage-full guard.
# 1. Start from main app mode.
# 2. Repeatedly upload valid MP3 files with fixed, allowable size.
# 3. Continue until server returns `507 Insufficient Storage`.
# 4. Assert failure message reports `Need` and `free` bytes consistently.
# 5. Verify the rejected file was not created.
# 6. If mount stats are available from logs, sanity-check consumed storage range.
def test_upload_until_storage_full_matches_capacity(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    # Size uploads from mount stats so we hit storage-full quickly without
    # relying on very long transfers on slow QEMU networking/filesystem paths.
    upload_size = 256 * 1024
    payload = _sized_mp3_payload(upload_size)
    start_stats = _extract_littlefs_mount_stats(log_path)
    start_free = None
    max_attempts = 32
    if start_stats is not None:
        total_bytes, used_bytes = start_stats
        start_free = max(0, total_bytes - used_bytes)
        # Target roughly 3-5 successful uploads before running out of space.
        computed_size = (start_free - (16 * 1024)) // 4
        upload_size = max(64 * 1024, computed_size)
        upload_size = min(upload_size, 512 * 1024)
        upload_size = min(upload_size, 2 * 1024 * 1024)
        payload = _sized_mp3_payload(upload_size)

        estimated_successes = max(1, start_free // max(upload_size, 1))
        max_attempts = min(64, estimated_successes + 3)

    success_count = 0
    failing_name = ""
    failing_status = None
    failing_body = ""

    for i in range(1, max_attempts + 1):
        name = f"{_unique_name('fill')}-{i:03d}.mp3"
        status, headers, body = _upload_sound(base_url, name, payload, timeout_s=45.0)
        if status == 303:
            assert headers.get("Location") == "/sounds/"
            success_count += 1
            continue

        failing_name = name
        failing_status = status
        failing_body = body
        break

    assert success_count > 0, "Expected at least one successful upload before storage-full."
    assert failing_status == 507, (
        "Expected storage-full rejection with 507.\n"
        f"successful_uploads={success_count}\n"
        f"last_status={failing_status}\n"
        f"last_body={failing_body}"
    )
    assert success_count < max_attempts, (
        "Reached upload-attempt cap before storage-full; "
        "adjust upload size or capacity estimates."
    )
    assert "Not enough storage space" in failing_body

    match = re.search(r"Need (\d+) bytes, only (\d+) bytes free\.", failing_body)
    assert match, f"Could not parse storage-full payload details from body:\n{failing_body}"

    required_bytes = int(match.group(1))
    free_bytes = int(match.group(2))
    expected_required = upload_size + (16 * 1024)
    assert required_bytes == expected_required, (
        f"Unexpected required byte count. expected={expected_required}, got={required_bytes}"
    )
    assert free_bytes < required_bytes

    _assert_sound_download_status(base_url, failing_name, 404)

    if start_free is not None:
        consumed = start_free - free_bytes
        min_expected = success_count * upload_size
        max_expected = success_count * (upload_size + (16 * 1024)) + (256 * 1024)

        assert consumed >= min_expected, (
            f"Consumed bytes lower than uploaded payload bytes. consumed={consumed}, min={min_expected}"
        )
        assert consumed <= max_expected, (
            f"Consumed bytes far above expected range. consumed={consumed}, max={max_expected}"
        )


# Test: Uploaded file can be deleted successfully.
# 1. Start from main app mode.
# 2. Upload a valid MP3 file.
# 3. Call `GET /sounds/<name>.mp3?delete=1`.
# 4. Assert delete redirect response.
# 5. Verify file is no longer downloadable.
def test_delete_uploaded_sound_file(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('delete')}.mp3"

    status, headers, body = _upload_sound(base_url, filename, _test_mp3_bytes())
    assert status == 303
    assert headers.get("Location") == "/sounds/"
    assert "File uploaded successfully" in body
    _assert_sound_download_status(base_url, filename, 200)

    del_status, del_headers, del_body = _http_get(base_url, f"/sounds/{filename}?delete=1")
    assert del_status == 303
    assert del_headers.get("Location") == "/sounds/"
    assert "File deleted successfully" in del_body
    _assert_sound_download_status(base_url, filename, 404)


# Test: Rename updates file name.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Call `GET /sounds/<old>.mp3?rename=<new-base>`.
# 4. Assert rename success response.
# 5. Verify old file is gone and new file exists.
def test_rename_file(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    old_name = f"{_unique_name('rename-src')}.mp3"
    new_base = _unique_name("rename-dst")
    new_name = f"{new_base}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, old_name, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for rename test. body={upload_body}"

    status, _, body = _http_get(base_url, f"/sounds/{old_name}?rename={new_base}")
    assert status == 200
    assert "Renamed" in body

    _assert_sound_download_status(base_url, old_name, 404)
    _assert_sound_download_status(base_url, new_name, 200)


# Test: Rename rejects too-large target name.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Call rename with oversized target base name.
# 4. Assert rejection response.
# 5. Verify no file was created under the oversized target name.
# 6. Verify source file still exists.
def test_reject_rename_to_too_large_name(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    old_name = f"{_unique_name('rename-too-long-src')}.mp3"
    # Keep this oversized for rename target validation, but short enough so
    # `/sounds/<name>` lookup itself remains a valid request path.
    long_base = "x" * 66
    invalid_new_name = f"{long_base}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, old_name, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for rename-length test. body={upload_body}"

    status, _, body = _http_get(base_url, f"/sounds/{old_name}?rename={long_base}")
    assert status in (400, 500)
    assert ("too long" in body.lower()) or ("Rename failed" in body)

    _assert_sound_download_status(base_url, invalid_new_name, 404)
    _assert_sound_download_status(base_url, old_name, 200)


# Test: Rename rejects empty target name.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Call rename with an empty `rename` query value.
# 4. Assert rejection response.
# 5. Verify source file still exists.
def test_reject_rename_to_empty_name(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    old_name = f"{_unique_name('rename-empty-src')}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, old_name, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for rename-empty test. body={upload_body}"

    status, _, body = _http_get(base_url, f"/sounds/{old_name}?rename=")
    assert status == 400
    assert "Rename target missing" in body
    _assert_sound_download_status(base_url, old_name, 200)


# Test: Metadata allows changing probability.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Set `probability` via `POST /sounds/file-meta/<name>`.
# 4. Fetch metadata and verify probability changed.
def test_set_sound_probability(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('meta-prob')}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, filename, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for probability test. body={upload_body}"

    before = _get_sound_meta(base_url, filename)
    target_probability = 42
    status, _, body = _set_sound_meta(base_url, filename, {"probability": target_probability})

    assert status == 200
    assert body == "OK"

    after = _get_sound_meta(base_url, filename)
    assert after["probability"] == target_probability
    assert after["volume"] == before["volume"]
    assert after["enabled"] == before["enabled"]


# Test: Metadata rejects invalid probabilities.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Send out-of-range probability in metadata update.
# 4. Assert request is rejected.
# 5. Verify metadata stayed unchanged.
def test_reject_invalid_sound_probability(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('meta-prob-invalid')}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, filename, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for invalid probability test. body={upload_body}"

    before = _get_sound_meta(base_url, filename)
    status, _, body = _set_sound_meta(base_url, filename, {"probability": 101})

    assert status == 400
    assert ("Probability out of range" in body) or ("Invalid probability" in body)

    after = _get_sound_meta(base_url, filename)
    assert after == before


# Test: Metadata allows changing volume.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Set `volume` via `POST /sounds/file-meta/<name>`.
# 4. Fetch metadata and verify volume changed.
def test_set_sound_volume(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('meta-vol')}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, filename, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for volume test. body={upload_body}"

    before = _get_sound_meta(base_url, filename)
    target_volume = 77
    status, _, body = _set_sound_meta(base_url, filename, {"volume": target_volume})

    assert status == 200
    assert body == "OK"

    after = _get_sound_meta(base_url, filename)
    assert after["volume"] == target_volume
    assert after["probability"] == before["probability"]
    assert after["enabled"] == before["enabled"]


# Test: Metadata rejects invalid volumes.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Send out-of-range volume in metadata update.
# 4. Assert request is rejected.
# 5. Verify metadata stayed unchanged.
def test_reject_invalid_sound_volume(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    filename = f"{_unique_name('meta-vol-invalid')}.mp3"

    upload_status, _, upload_body = _upload_sound(base_url, filename, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for invalid volume test. body={upload_body}"

    before = _get_sound_meta(base_url, filename)
    status, _, body = _set_sound_meta(base_url, filename, {"volume": 126})

    assert status == 400
    assert ("Volume out of range" in body) or ("Invalid volume" in body)

    after = _get_sound_meta(base_url, filename)
    assert after == before


# Test: Rename + probability + volume survive reboot.
# 1. Start from main app mode.
# 2. Upload source file.
# 3. Rename file and update `probability` + `volume`.
# 4. Restart device back to bootstrap, then enter main app again.
# 5. Verify new name exists, old name is gone, and metadata persisted.
def test_rename_and_meta_persist_after_reboot(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    old_name = f"{_unique_name('persist-src')}.mp3"
    new_base = _unique_name("persist-dst")
    new_name = f"{new_base}.mp3"
    expected_probability = 23
    expected_volume = 117

    upload_status, _, upload_body = _upload_sound(base_url, old_name, _test_mp3_bytes())
    assert upload_status == 303, f"Upload setup failed for persistence test. body={upload_body}"

    rename_status, _, rename_body = _http_get(base_url, f"/sounds/{old_name}?rename={new_base}")
    assert rename_status == 200
    assert "Renamed" in rename_body

    meta_status, _, meta_body = _set_sound_meta(
        base_url,
        new_name,
        {"probability": expected_probability, "volume": expected_volume},
    )
    assert meta_status == 200
    assert meta_body == "OK"

    _restart_into_bootstrap(base_url, log_path)
    _skip_to_main_app(base_url, log_path)

    _assert_sound_download_status(base_url, old_name, 404)
    _assert_sound_download_status(base_url, new_name, 200)

    persisted = _get_sound_meta(base_url, new_name)
    assert persisted["probability"] == expected_probability
    assert persisted["volume"] == expected_volume


# Test: Upload rejects empty filename.
# 1. Start from main app mode.
# 2. Call `POST /sounds/` with payload but no filename.
# 3. Call `POST /sounds/.mp3` (empty base name with extension only).
# 4. Assert both are rejected.
# 5. Verify `.mp3` was not created.
def test_reject_empty_file_name(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]

    status_empty, _, body_empty = _http_request(
        base_url=base_url,
        method="POST",
        path="/sounds/",
        timeout_s=3.0,
        data=_test_mp3_bytes(),
        headers={"Content-Type": "audio/mpeg"},
    )

    status_dot, _, body_dot = _http_request(
        base_url=base_url,
        method="POST",
        path="/sounds/.mp3",
        timeout_s=3.0,
        data=_test_mp3_bytes(),
        headers={"Content-Type": "audio/mpeg"},
    )

    assert status_empty == 400
    assert "Invalid filename" in body_empty
    assert status_dot == 400
    assert ("Invalid filename" in body_dot) or ("Only audio files are allowed" in body_dot)
    _assert_sound_download_status(base_url, ".mp3", 404)
