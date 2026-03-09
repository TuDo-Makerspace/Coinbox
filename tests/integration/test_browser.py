from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

import pytest

try:
    from tests.integration.integration_helpers import (
        BOOT_TIMEOUT_S,
        CUSTOM_UI_PASSWORD,
        _enter_recovery_mode,
        _expected_recovery_code,
        _get_qemu_factory_mac,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _log_contains_any_since,
        _restart_into_bootstrap,
        _set_security_password,
        _skip_to_main_app,
        _stop_process_group,
        _tail_log,
        _test_mp3_bytes,
        _wait_for_bootstrap_countdown_threshold,
        _wait_until,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        BOOT_TIMEOUT_S,
        CUSTOM_UI_PASSWORD,
        _enter_recovery_mode,
        _expected_recovery_code,
        _get_qemu_factory_mac,
        _http_get,
        _http_request,
        _is_bootstrap_root_page,
        _log_contains_any_since,
        _restart_into_bootstrap,
        _set_security_password,
        _skip_to_main_app,
        _stop_process_group,
        _tail_log,
        _test_mp3_bytes,
        _wait_for_bootstrap_countdown_threshold,
        _wait_until,
        qemu_bootstrap_instance,
    )


@pytest.fixture
def qemu_mainapp_instance(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    _skip_to_main_app(base_url, log_path)
    return qemu_bootstrap_instance


DEFAULT_SOUND_FILENAME = "default.mp3"
HEADLESS_PAGE_CAPTURE_TIMEOUT_S = 15.0
HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S = 20.0
HANDOFF_DELAY_TOLERANCE_S = 0.5


def _capture_browser_state_in_headless_chrome(
    url: str,
    wait_paths: tuple[str, ...] = (),
    wait_condition=None,
    wait_s: float = HEADLESS_PAGE_CAPTURE_TIMEOUT_S,
) -> dict:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                last_state = {
                    "current_url": url,
                    "current_path": urllib.parse.urlparse(url).path or "/",
                    "title": "",
                    "body_text": "",
                    "page_source": "",
                    "page_boot_id": "",
                    "has_connection_lost_overlay": False,
                    "connection_lost_visible": False,
                    "error": "",
                }
                deadline = time.time() + wait_s
                while time.time() < deadline:
                    try:
                        state = _cdp_capture_page_state(sock, next_id)
                        next_id += 1
                        if state:
                            last_state = state
                            if wait_paths and _state_matches_interactive_wait_path(state, wait_paths):
                                break
                            if wait_condition is not None and wait_condition(state):
                                break
                    except Exception as exc:
                        last_state["error"] = f"{type(exc).__name__}: {exc}"
                    time.sleep(0.2)
            finally:
                try:
                    sock.close()
                except Exception:
                    pass

            return last_state
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _capture_sounds_notice_transition_states(
    base_url: str,
    *,
    ready_row_name: str,
    trigger_expression: str | None = None,
    after_ready=None,
    wait_s: float = 6.0,
) -> list[dict]:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    url = f"{base_url}/sounds/"
    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                ready_deadline = time.time() + HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S
                initial_state = {}
                while time.time() < ready_deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    initial_state = state
                    if (
                        state.get("current_path") == "/sounds/"
                        and state.get("page_ready") == "1"
                        and ready_row_name in state.get("body_text", "")
                    ):
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError(
                        "Sounds page did not become interactive before playback-notice trigger.\n"
                        f"Last state: {initial_state}"
                    )

                _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "(() => {"
                            "  window.alert = () => {};"
                            "  return true;"
                            "})()"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1

                captured_states: list[dict] = [initial_state]
                if after_ready is not None:
                    after_ready()

                if trigger_expression:
                    trigger_response = _cdp_send_command(
                        sock,
                        next_id,
                        "Runtime.evaluate",
                        {
                            "expression": trigger_expression,
                            "returnByValue": True,
                        },
                    )
                    next_id += 1
                    trigger_value = trigger_response.get("result", {}).get("result", {}).get("value")
                    assert trigger_value not in ("missing-row", "missing-play-button"), (
                        f"Sounds-page playback trigger could not find the target row/button: {trigger_value!r}"
                    )

                deadline = time.time() + wait_s
                while time.time() < deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    state["elapsed_s"] = wait_s - max(0.0, deadline - time.time())
                    captured_states.append(state)
                    time.sleep(0.15)

                return captured_states
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _browser_state_details(browser_state: dict, log_path) -> str:
    return (
        f"Browser URL: {browser_state.get('current_url')}\n"
        f"Browser title: {browser_state.get('title')}\n"
        f"Browser boot id: {browser_state.get('page_boot_id')}\n"
        f"Browser error: {browser_state.get('error')}\n"
        f"Body text:\n{browser_state.get('body_text', '')[:1200]}\n"
        f"DOM snippet:\n{browser_state.get('page_source', '')[:1200]}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


def _assert_browser_lands_on(
    browser_state: dict,
    expected_path: str,
    expected_title_fragment: str,
    log_path,
    message: str,
):
    details = _browser_state_details(browser_state, log_path)
    assert browser_state.get("current_path") == expected_path, f"{message}\n{details}"
    assert expected_title_fragment.lower() in browser_state.get("title", "").lower(), (
        f"{message}\n{details}"
    )


def _wait_for_connection_lost_popup_after_disconnect(
    state: dict,
    proc: subprocess.Popen,
    disconnect_triggered: dict,
    expected_path: str,
    expected_title_fragment: str,
) -> bool:
    if not disconnect_triggered["done"]:
        if (
            state.get("current_path") == expected_path
            and expected_title_fragment.lower() in state.get("title", "").lower()
            and state.get("has_connection_lost_overlay") is True
        ):
            _stop_process_group(proc)
            disconnect_triggered["done"] = True
        return False

    return state.get("connection_lost_visible") is True


def _assert_connection_lost_popup_visible(
    browser_state: dict,
    disconnect_triggered: dict,
    log_path,
    message: str,
):
    details = _browser_state_details(browser_state, log_path)
    assert disconnect_triggered["done"], f"{message}\n{details}"
    assert browser_state.get("has_connection_lost_overlay") is True, f"{message}\n{details}"
    assert browser_state.get("connection_lost_visible") is True, f"{message}\n{details}"
    assert "Connection lost!" in browser_state.get("body_text", ""), f"{message}\n{details}"


def _body_text_matches(browser_state: dict, pattern: str) -> bool:
    return re.search(pattern, browser_state.get("body_text", ""), flags=re.IGNORECASE) is not None


def _first_matching_state_index(states: list[dict], pattern: str) -> int | None:
    for idx, state in enumerate(states):
        if _body_text_matches(state, pattern):
            return idx
    return None


def _http_post_json(base_url: str, path: str, payload: dict):
    return _http_request(
        base_url=base_url,
        method="POST",
        path=path,
        timeout_s=4.0,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )


def _set_audio_config(base_url: str, payload: dict) -> dict:
    status, headers, body = _http_post_json(base_url, "/audio/config", payload)
    assert status == 200, (
        f"Expected 200 from POST /audio/config, got {status}. "
        f"content-type={headers.get('Content-Type', '')} body={body}"
    )
    assert "application/json" in headers.get("Content-Type", ""), (
        f"/audio/config did not return JSON. content-type={headers.get('Content-Type', '')}"
    )
    parsed = json.loads(body)
    assert isinstance(parsed, dict), f"/audio/config did not return a JSON object: {parsed!r}"
    return parsed


def _set_test_gpio_level(base_url: str, name: str, level: int):
    status, headers, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/test/gpio/{name}?level={level}",
        timeout_s=4.0,
        data=b"",
    )
    assert status == 200, (
        f"Expected 200 from POST /test/gpio/{name}?level={level}, got {status}. "
        f"content-type={headers.get('Content-Type', '')} body={body}"
    )
    assert "application/json" in headers.get("Content-Type", ""), (
        f"/test/gpio/{name} did not return JSON. content-type={headers.get('Content-Type', '')}"
    )
    payload = json.loads(body)
    assert payload.get("level") == level, f"Unexpected GPIO echo for {name}: {payload}"


def _trigger_test_laser_playback(base_url: str):
    status, headers, body = _http_request(
        base_url=base_url,
        method="POST",
        path="/test/gpio/laser-burst?count=1&interval_ms=0",
        timeout_s=8.0,
        data=b"",
    )
    assert status == 200, (
        f"Expected 200 from POST /test/gpio/laser-burst, got {status}. "
        f"content-type={headers.get('Content-Type', '')} body={body}"
    )
    assert "application/json" in headers.get("Content-Type", ""), (
        "/test/gpio/laser-burst did not return JSON. "
        f"content-type={headers.get('Content-Type', '')}"
    )
    payload = json.loads(body)
    assert payload.get("count") == 1, f"Unexpected laser burst payload: {payload}"
    assert payload.get("level") == 1, f"Unexpected laser level after burst: {payload}"


def _set_sound_meta(base_url: str, filename: str, payload: dict):
    status, headers, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/file-meta/{filename}",
        timeout_s=4.0,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    assert status == 200, (
        f"Expected 200 from POST /sounds/file-meta/{filename}, got {status}. "
        f"content-type={headers.get('Content-Type', '')} body={body}"
    )


def _upload_sound_fixture(base_url: str, filename: str):
    status, headers, body = _http_request(
        base_url=base_url,
        method="POST",
        path=f"/sounds/{filename}",
        timeout_s=10.0,
        data=_test_mp3_bytes(),
        headers={"Content-Type": "audio/mpeg"},
    )
    assert status == 303, (
        f"Expected upload of {filename} to redirect, got {status}. "
        f"content-type={headers.get('Content-Type', '')} body={body}"
    )


def _get_runtime_status(base_url: str, timeout_s: float = 2.0) -> dict:
    status, headers, body = _http_get(base_url, "/runtime/status", timeout_s=timeout_s)
    assert status == 200, (
        f"Expected 200 from /runtime/status, got {status}. "
        f"content-type={headers.get('Content-Type', '')} body={body}"
    )
    assert "application/json" in headers.get("Content-Type", ""), (
        f"/runtime/status did not return JSON. content-type={headers.get('Content-Type', '')}"
    )
    payload = json.loads(body)
    assert isinstance(payload, dict), f"/runtime/status did not return a JSON object: {payload!r}"
    boot_id = payload.get("boot_id")
    assert isinstance(boot_id, str) and boot_id, f"/runtime/status did not return a non-empty boot_id: {payload!r}"
    return payload


def _try_get_runtime_status(base_url: str, timeout_s: float = 0.5) -> dict | None:
    try:
        status, headers, body = _http_get(base_url, "/runtime/status", timeout_s=timeout_s)
    except Exception:
        return None

    if status != 200 or "application/json" not in headers.get("Content-Type", ""):
        return None

    try:
        payload = json.loads(body)
    except Exception:
        return None

    if not isinstance(payload, dict):
        return None

    boot_id = payload.get("boot_id")
    if not isinstance(boot_id, str) or not boot_id:
        return None

    return payload


def _capture_recovery_exit_transition_states(base_url: str) -> list[dict]:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    url = f"{base_url}/"
    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                ready_deadline = time.time() + HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S
                last_state = {}
                while time.time() < ready_deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    last_state = state
                    if (
                        state.get("current_path") == "/"
                        and state.get("page_ready") == "1"
                        and "Recovery Mode" in state.get("body_text", "")
                        and "Exit recovery mode" in state.get("body_text", "")
                    ):
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError(f"Recovery page did not become interactive before click.\nLast state: {last_state}")

                click_response = _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "(() => {"
                            "  const btn = document.getElementById('start-now');"
                            "  if (!btn) return 'missing-button';"
                            "  btn.click();"
                            "  return btn.textContent || '';"
                            "})()"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1
                click_value = click_response.get("result", {}).get("result", {}).get("value")
                assert click_value != "missing-button", "Recovery exit button was not present in the browser DOM."

                captured_states: list[dict] = []
                deadline = time.time() + 3.0
                while time.time() < deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    captured_states.append(state)
                    if _state_matches_interactive_wait_path(state, ("/sounds/",)):
                        break
                    time.sleep(0.05)

                return captured_states
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _capture_start_now_transition_states(base_url: str) -> list[dict]:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    url = f"{base_url}/"
    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                ready_deadline = time.time() + HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S
                last_state = {}
                while time.time() < ready_deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    last_state = state
                    if state.get("current_path") == "/" and state.get("page_ready") == "1":
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError(f"Bootstrap page did not become interactive before click.\nLast state: {last_state}")

                click_response = _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "(() => {"
                            "  const btn = document.getElementById('start-now');"
                            "  if (!btn) return 'missing-button';"
                            "  btn.click();"
                            "  return btn.textContent || '';"
                            "})()"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1
                click_value = click_response.get("result", {}).get("result", {}).get("value")
                assert click_value != "missing-button", "Bootstrap start-now button was not present in the browser DOM."

                captured_states: list[dict] = []
                deadline = time.time() + 3.0
                while time.time() < deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    captured_states.append(state)
                    if _state_matches_interactive_wait_path(state, ("/sounds/", "/login")):
                        break
                    time.sleep(0.05)

                return captured_states
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _capture_handoff_disconnect_states(base_url: str, proc: subprocess.Popen, recovery_mode: bool) -> list[dict]:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    url = f"{base_url}/"
    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                ready_deadline = time.time() + HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S
                last_state = {}
                while time.time() < ready_deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    last_state = state
                    if recovery_mode:
                        if (
                            state.get("current_path") == "/"
                            and state.get("page_ready") == "1"
                            and "Recovery Mode" in state.get("body_text", "")
                            and "Exit recovery mode" in state.get("body_text", "")
                        ):
                            break
                    else:
                        if (
                            state.get("current_path") == "/"
                            and state.get("page_ready") == "1"
                            and "Coinbox is starting" in state.get("body_text", "")
                            and "Start now" in state.get("body_text", "")
                        ):
                            break
                    time.sleep(0.05)
                else:
                    mode_label = "recovery page" if recovery_mode else "bootstrap page"
                    raise AssertionError(
                        f"The {mode_label} did not become interactive before the handoff click.\n"
                        f"Last state: {last_state}"
                    )

                click_response = _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "(() => {"
                            "  const btn = document.getElementById('start-now');"
                            "  if (!btn) return 'missing-button';"
                            "  btn.click();"
                            "  return btn.textContent || '';"
                            "})()"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1
                click_value = click_response.get("result", {}).get("result", {}).get("value")
                assert click_value != "missing-button", "Handoff button was not present in the browser DOM."

                captured_states: list[dict] = []
                disconnect_triggered = False
                started_at = time.time()
                deadline = started_at + 10.0
                while time.time() < deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    state["elapsed_s"] = time.time() - started_at
                    captured_states.append(state)

                    if not disconnect_triggered:
                        if (
                            state.get("current_path") == "/"
                            and "Starting main application" in state.get("body_text", "")
                            and "Getting things ready..." in state.get("body_text", "")
                        ):
                            _stop_process_group(proc)
                            disconnect_triggered = True
                    elif state.get("connection_lost_visible") is True:
                        break

                    time.sleep(0.05)

                return captured_states
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _capture_settings_restart_action_states(base_url: str, action_button_id: str, log_path) -> list[dict]:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    url = f"{base_url}/settings"
    initial_runtime_status = _get_runtime_status(base_url)
    initial_boot_id = str(initial_runtime_status.get("boot_id", ""))
    log_start_pos = log_path.stat().st_size if log_path and log_path.exists() else 0
    reboot_markers = [
        "rst:0x1 (POWERON_RESET)",
        "rst:0x3 (SW_RESET)",
        "rst:0xc (SW_CPU_RESET)",
        "main_task: Calling app_main()",
        "bootstrap: Bootstrap server started",
    ]
    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                ready_deadline = time.time() + HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S
                last_state = {}
                while time.time() < ready_deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    last_state = state
                    if (
                        state.get("current_path") == "/settings"
                        and state.get("page_ready") == "1"
                        and state.get("page_boot_id") == initial_boot_id
                        and "Settings" in state.get("body_text", "")
                    ):
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError(
                        "Settings page did not become interactive before restart-action click.\n"
                        f"Last state: {last_state}"
                    )

                _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "(() => {"
                            "  window.confirm = () => true;"
                            "  window.alert = () => {};"
                            "  return true;"
                            "})()"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1

                click_response = _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "((buttonId) => {"
                            "  const btn = document.getElementById(buttonId);"
                            "  if (!btn) return 'missing-button';"
                            "  btn.click();"
                            "  return btn.textContent || buttonId;"
                            f"}})({json.dumps(action_button_id)})"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1
                click_value = click_response.get("result", {}).get("result", {}).get("value")
                assert click_value != "missing-button", (
                    f"Settings restart action button {action_button_id!r} was not present in the browser DOM."
                )

                captured_states: list[dict] = []
                started_at = time.time()
                deadline = started_at + 20.0
                while time.time() < deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    runtime_status = _try_get_runtime_status(base_url)
                    runtime_boot_id = ""
                    if runtime_status is not None:
                        runtime_boot_id = str(runtime_status.get("boot_id", ""))
                    state["elapsed_s"] = time.time() - started_at
                    state["initial_boot_id"] = initial_boot_id
                    state["runtime_boot_id"] = runtime_boot_id
                    state["new_boot_id_seen"] = bool(runtime_boot_id) and runtime_boot_id != initial_boot_id
                    state["reboot_seen"] = _log_contains_any_since(log_path, log_start_pos, reboot_markers)
                    state["site_reconnected"] = (
                        state.get("new_boot_id_seen") is True
                        and state.get("page_boot_id") == runtime_boot_id
                        and state.get("connection_lost_visible") is not True
                    )
                    state["site_ready"] = (
                        state.get("site_reconnected") is True
                        and state.get("page_ready") == "1"
                    )
                    captured_states.append(state)
                    if state.get("site_ready") is True:
                        break
                    time.sleep(0.05)

                return captured_states
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _transition_states_details(states: list[dict], log_path) -> str:
    lines = []
    for index, state in enumerate(states[:20], start=1):
        body = state.get("body_text", "").replace("\n", " | ")
        lines.append(
            f"{index}. path={state.get('current_path')} title={state.get('title')} body={body[:220]}"
        )
    return "Captured states:\n" + "\n".join(lines) + f"\nLog tail:\n{_tail_log(log_path)}"


def _state_matches_interactive_wait_path(state: dict, wait_paths: tuple[str, ...]) -> bool:
    if state.get("current_path") not in wait_paths:
        return False

    # Ignore transient captures taken mid-navigation before the destination page
    # has populated its DOM and declared itself ready.
    if not (state.get("title") or state.get("body_text") or state.get("page_source")):
        return False

    page_ready = str(state.get("page_ready", "") or "")
    if page_ready and page_ready != "1":
        return False

    return True


def _timed_transition_states_details(states: list[dict], log_path) -> str:
    def _format_state(index: int, state: dict) -> str:
        body = state.get("body_text", "").replace("\n", " | ")
        return (
            f"{index}. t={float(state.get('elapsed_s', 0.0)):.2f}s"
            f" path={state.get('current_path')}"
            f" overlay={state.get('connection_lost_visible')}"
            f" reboot_seen={state.get('reboot_seen')}"
            f" page_boot_id={state.get('page_boot_id')}"
            f" runtime_boot_id={state.get('runtime_boot_id')}"
            f" new_boot_id={state.get('new_boot_id_seen')}"
            f" reconnected={state.get('site_reconnected')}"
            f" ready={state.get('site_ready')}"
            f" body={body[:220]}"
        )

    lines = []
    if len(states) <= 24:
        for index, state in enumerate(states, start=1):
            lines.append(_format_state(index, state))
    else:
        for index, state in enumerate(states[:12], start=1):
            lines.append(_format_state(index, state))
        lines.append("...")
        tail_offset = len(states) - 12
        for index, state in enumerate(states[-12:], start=tail_offset + 1):
            lines.append(_format_state(index, state))
    return "Captured states:\n" + "\n".join(lines) + f"\nLog tail:\n{_tail_log(log_path)}"


def _extract_main_handoff_connection_monitor_delay_ms(html: str) -> int | None:
    match = re.search(r"const\s+MAIN_HANDOFF_CONNECTION_MONITOR_DELAY_MS\s*=\s*(\d+)\s*;", html)
    if not match:
        return None
    return int(match.group(1))


def _state_shows_handoff_card(state: dict) -> bool:
    body = state.get("body_text", "")
    return (
        state.get("current_path") == "/"
        and "Starting main application" in body
        and "Getting things ready..." in body
    )


def _state_shows_native_connection_error(state: dict) -> bool:
    body = state.get("body_text", "")
    return (
        "ERR_CONNECTION_REFUSED" in body
        or ("This site can" in body and "refused to connect" in body)
    )


def _find_browser_binary() -> str | None:
    env_path = os.environ.get("CHROME_BIN", "").strip()
    if env_path:
        resolved_env = os.path.realpath(env_path)
        if os.path.isfile(resolved_env) and os.access(resolved_env, os.X_OK):
            return resolved_env

    for candidate in ("google-chrome", "google-chrome-stable", "chromium-browser", "chromium"):
        path = shutil.which(candidate)
        if not path:
            continue
        resolved = os.path.realpath(path)
        sibling = os.path.join(os.path.dirname(resolved), "chrome")
        if os.path.isfile(sibling) and os.access(sibling, os.X_OK):
            return sibling
        if os.path.isfile(resolved) and os.access(resolved, os.X_OK):
            return resolved
    return None


def _reserve_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _cdp_browser_ready(browser: subprocess.Popen, version_url: str) -> bool:
    if browser.poll() is not None:
        return False
    try:
        _http_json(version_url)
        return True
    except Exception:
        return False


def _http_json(url: str, method: str = "GET") -> dict:
    req = urllib.request.Request(url, method=method)
    with urllib.request.urlopen(req, timeout=3.0) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _read_process_stderr(proc: subprocess.Popen) -> str:
    if not proc.stderr:
        return ""
    if proc.poll() is None:
        return ""
    try:
        return proc.stderr.read()
    except Exception:
        return ""


def _recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("WebSocket closed unexpectedly.")
        chunks.extend(chunk)
    return bytes(chunks)


def _ws_connect(ws_url: str) -> socket.socket:
    parsed = urllib.parse.urlparse(ws_url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"

    sock = socket.create_connection((host, port), timeout=5.0)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))

    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("WebSocket handshake failed.")
        response.extend(chunk)

    headers = response.decode("latin1", errors="replace")
    assert "101" in headers.splitlines()[0], f"Unexpected WebSocket handshake response: {headers}"
    expected_accept = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
    ).decode("ascii")
    assert f"Sec-WebSocket-Accept: {expected_accept}" in headers, (
        "WebSocket handshake accept header mismatch.\n"
        f"Response headers:\n{headers}"
    )
    return sock


def _ws_send_json(sock: socket.socket, payload: dict):
    body = json.dumps(payload).encode("utf-8")
    mask = os.urandom(4)
    header = bytearray([0x81])
    body_len = len(body)
    if body_len < 126:
        header.append(0x80 | body_len)
    elif body_len < 65536:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", body_len))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", body_len))
    header.extend(mask)
    masked_body = bytes(byte ^ mask[i % 4] for i, byte in enumerate(body))
    sock.sendall(bytes(header) + masked_body)


def _ws_recv_text(sock: socket.socket, timeout_s: float) -> str | None:
    sock.settimeout(timeout_s)
    while True:
        first_two = _recv_exact(sock, 2)
        opcode = first_two[0] & 0x0F
        masked = (first_two[1] & 0x80) != 0
        length = first_two[1] & 0x7F
        if length == 126:
            length = struct.unpack("!H", _recv_exact(sock, 2))[0]
        elif length == 127:
            length = struct.unpack("!Q", _recv_exact(sock, 8))[0]
        mask = _recv_exact(sock, 4) if masked else b""
        payload = _recv_exact(sock, length)
        if masked:
            payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))

        if opcode == 0x8:
            return None
        if opcode == 0x9:
            _ws_send_control(sock, 0xA, payload)
            continue
        if opcode == 0xA:
            continue
        if opcode == 0x1:
            return payload.decode("utf-8", errors="replace")


def _ws_send_control(sock: socket.socket, opcode: int, payload: bytes):
    header = bytearray([0x80 | opcode])
    payload_len = len(payload)
    if payload_len < 126:
        header.append(0x80 | payload_len)
    else:
        raise ValueError("Control frame payload too large.")
    mask = os.urandom(4)
    header.extend(mask)
    masked_payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(bytes(header) + masked_payload)


def _cdp_send_command(sock: socket.socket, command_id: int, method: str, params: dict | None = None) -> dict:
    _ws_send_json(sock, {"id": command_id, "method": method, "params": params or {}})
    deadline = time.time() + 5.0
    while time.time() < deadline:
        message = _ws_recv_text(sock, max(0.1, deadline - time.time()))
        if message is None:
            raise ConnectionError("DevTools websocket closed.")
        data = json.loads(message)
        if data.get("id") == command_id:
            return data
    raise TimeoutError(f"Timed out waiting for CDP response to {method}.")


def _cdp_capture_page_state(sock: socket.socket, command_id: int) -> dict:
    expression = (
        "(() => ({"
        "href: window.location.href,"
        "path: window.location.pathname || '/',"
        "title: document.title || '',"
        "html: document.documentElement ? document.documentElement.outerHTML.slice(0, 12000) : '',"
        "bodyText: document.body ? document.body.innerText.slice(0, 4000) : '',"
        "pageBootId: document.documentElement && document.documentElement.dataset"
        "  ? document.documentElement.dataset.bootId || ''"
        "  : '',"
        "pageReady: document.documentElement && document.documentElement.dataset"
        "  ? document.documentElement.dataset.pageReady || ''"
        "  : '',"
        "hasConnectionLostOverlay: !!document.getElementById('coinbox-connection-lost-overlay')"
        "  || !!document.getElementById('connection-lost-overlay'),"
        "connectionLostVisible: (() => {"
        "  const overlay = document.getElementById('coinbox-connection-lost-overlay')"
        "    || document.getElementById('connection-lost-overlay');"
        "  return !!overlay && overlay.dataset && overlay.dataset.visible === '1';"
        "})()"
        "}))()"
    )
    response = _cdp_send_command(
        sock,
        command_id,
        "Runtime.evaluate",
        {
            "expression": expression,
            "returnByValue": True,
        },
    )
    result = response.get("result", {}).get("result", {})
    value = result.get("value")
    if not isinstance(value, dict):
        raise RuntimeError(f"Unexpected CDP evaluate response: {response}")
    current_url = str(value.get("href", ""))
    current_path = str(value.get("path", "")) or "/"
    return {
        "current_url": current_url,
        "current_path": current_path,
        "title": str(value.get("title", "")),
        "body_text": str(value.get("bodyText", "")),
        "page_source": str(value.get("html", "")),
        "page_boot_id": str(value.get("pageBootId", "")),
        "page_ready": str(value.get("pageReady", "")),
        "has_connection_lost_overlay": bool(value.get("hasConnectionLostOverlay", False)),
        "connection_lost_visible": bool(value.get("connectionLostVisible", False)),
        "error": "",
    }


def _cdp_capture_page_diagnostics(sock: socket.socket, command_id: int) -> dict:
    expression = (
        "(() => ({"
        "href: window.location.href,"
        "path: window.location.pathname || '/',"
        "title: document.title || '',"
        "html: document.documentElement ? document.documentElement.outerHTML.slice(0, 12000) : '',"
        "bodyText: document.body ? document.body.innerText.slice(0, 4000) : '',"
        "pageReady: document.documentElement && document.documentElement.dataset"
        "  ? document.documentElement.dataset.pageReady || ''"
        "  : '',"
        "scriptPaths: Array.from(document.scripts || [])"
        "  .map((script) => script && script.src ? new URL(script.src, window.location.href).pathname : '')"
        "  .filter(Boolean),"
        "paintTimings: performance.getEntriesByType('paint').map((entry) => ({"
        "  name: entry.name || '',"
        "  startTime: Number(entry.startTime || 0)"
        "})),"
        "resourceTimings: performance.getEntriesByType('resource').map((entry) => ({"
        "  name: entry.name || '',"
        "  initiatorType: entry.initiatorType || '',"
        "  startTime: Number(entry.startTime || 0),"
        "  responseEnd: Number(entry.responseEnd || 0),"
        "  duration: Number(entry.duration || 0)"
        "})),"
        "hasConnectionMonitorInstalled: !!window.__coinboxConnectionMonitorInstalled,"
        "hasGlyphApi: !!(window.COINBOX_GLYPHS && typeof window.COINBOX_GLYPHS.sprinkleBackgroundGlyphs === 'function'),"
        "hasGlyphLayer: !!document.querySelector('.bg-glyph-layer'),"
        "hasNavbarStyle: !!document.getElementById('navbar-shared-style'),"
        "navbarLinks: Array.from(document.querySelectorAll('#navbar nav a')).map((link) => (link.textContent || '').trim())"
        "}))()"
    )
    response = _cdp_send_command(
        sock,
        command_id,
        "Runtime.evaluate",
        {
            "expression": expression,
            "returnByValue": True,
        },
    )
    result = response.get("result", {}).get("result", {})
    value = result.get("value")
    if not isinstance(value, dict):
        raise RuntimeError(f"Unexpected CDP evaluate response: {response}")
    return {
        "current_url": str(value.get("href", "")),
        "current_path": str(value.get("path", "")) or "/",
        "title": str(value.get("title", "")),
        "body_text": str(value.get("bodyText", "")),
        "page_source": str(value.get("html", "")),
        "page_ready": str(value.get("pageReady", "")),
        "script_paths": [str(path) for path in value.get("scriptPaths", []) if str(path)],
        "paint_timings": list(value.get("paintTimings", [])),
        "resource_timings": list(value.get("resourceTimings", [])),
        "has_connection_monitor_installed": bool(value.get("hasConnectionMonitorInstalled", False)),
        "has_glyph_api": bool(value.get("hasGlyphApi", False)),
        "has_glyph_layer": bool(value.get("hasGlyphLayer", False)),
        "has_navbar_style": bool(value.get("hasNavbarStyle", False)),
        "navbar_links": [str(link) for link in value.get("navbarLinks", []) if str(link)],
        "error": "",
    }


def _script_paths_present(browser_diagnostics: dict, expected_script_paths: tuple[str, ...]) -> bool:
    script_paths = {str(path) for path in browser_diagnostics.get("script_paths", [])}
    return all(path in script_paths for path in expected_script_paths)


def _resource_timings_ready(browser_diagnostics: dict, expected_script_paths: tuple[str, ...]) -> bool:
    for script_path in expected_script_paths:
        matches = [
            entry
            for entry in browser_diagnostics.get("resource_timings", [])
            if _normalize_resource_path(str(entry.get("name", ""))) == script_path
        ]
        if not matches:
            return False
        if not any(float(entry.get("responseEnd", 0.0)) > 0.0 for entry in matches):
            return False
    return True


def _page_load_diagnostics_ready(
    browser_diagnostics: dict,
    expected_path: str,
    expected_script_paths: tuple[str, ...],
    require_navbar: bool = False,
) -> bool:
    if browser_diagnostics.get("current_path") != expected_path:
        return False
    if browser_diagnostics.get("page_ready") != "1":
        return False
    if _first_contentful_paint_ms(browser_diagnostics) is None:
        return False
    if not _script_paths_present(browser_diagnostics, expected_script_paths):
        return False
    if not _resource_timings_ready(browser_diagnostics, expected_script_paths):
        return False
    if browser_diagnostics.get("has_connection_monitor_installed") is not True:
        return False
    if browser_diagnostics.get("has_glyph_api") is not True:
        return False
    if browser_diagnostics.get("has_glyph_layer") is not True:
        return False
    if require_navbar:
        if browser_diagnostics.get("has_navbar_style") is not True:
            return False
        if browser_diagnostics.get("navbar_links") != ["Sounds", "Settings"]:
            return False
    return True


def _diagnostics_from_state(state: dict) -> dict:
    return {
        "current_url": str(state.get("current_url", "")),
        "current_path": str(state.get("current_path", "")) or "/",
        "title": str(state.get("title", "")),
        "body_text": str(state.get("body_text", "")),
        "page_source": str(state.get("page_source", "")),
        "page_ready": str(state.get("page_ready", "")),
        "script_paths": [],
        "paint_timings": [],
        "resource_timings": [],
        "has_connection_monitor_installed": False,
        "has_glyph_api": False,
        "has_glyph_layer": False,
        "has_navbar_style": False,
        "navbar_links": [],
        "error": str(state.get("error", "")),
    }


def _capture_settled_page_diagnostics(
    sock: socket.socket,
    next_id: int,
    last_state: dict,
    settle_condition=None,
    wait_s: float = 5.0,
    poll_s: float = 0.1,
) -> tuple[dict, int]:
    last_diagnostics: dict | None = None
    deadline = time.time() + wait_s
    while time.time() < deadline:
        try:
            state = _cdp_capture_page_state(sock, next_id)
            next_id += 1
            last_state = state

            diagnostics = _cdp_capture_page_diagnostics(sock, next_id)
            next_id += 1
            diagnostics["error"] = last_state.get("error", "")
            last_diagnostics = diagnostics

            if settle_condition is None or settle_condition(diagnostics):
                return diagnostics, next_id
        except Exception as exc:
            last_state["error"] = f"{type(exc).__name__}: {exc}"
        time.sleep(poll_s)

    if last_diagnostics is None:
        last_diagnostics = _diagnostics_from_state(last_state)
    last_diagnostics["error"] = last_state.get("error", "")
    return last_diagnostics, next_id


def _capture_page_load_diagnostics_in_headless_chrome(
    url: str,
    wait_paths: tuple[str, ...] = (),
    wait_condition=None,
    settle_condition=None,
    wait_s: float = HEADLESS_PAGE_CAPTURE_TIMEOUT_S,
) -> dict:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                last_state = {
                    "current_url": url,
                    "current_path": urllib.parse.urlparse(url).path or "/",
                    "title": "",
                    "body_text": "",
                    "page_source": "",
                    "error": "",
                }
                deadline = time.time() + wait_s
                while time.time() < deadline:
                    try:
                        state = _cdp_capture_page_state(sock, next_id)
                        next_id += 1
                        if state:
                            last_state = state
                            if wait_paths and _state_matches_interactive_wait_path(state, wait_paths):
                                break
                            if wait_condition is not None and wait_condition(state):
                                break
                    except Exception as exc:
                        last_state["error"] = f"{type(exc).__name__}: {exc}"
                    time.sleep(0.2)

                diagnostics, next_id = _capture_settled_page_diagnostics(
                    sock,
                    next_id,
                    last_state,
                    settle_condition=settle_condition,
                    wait_s=wait_s,
                )
                return diagnostics
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _capture_start_now_load_diagnostics(base_url: str) -> dict:
    chrome_binary = _find_browser_binary()
    if not chrome_binary:
        pytest.skip("Headless Chrome not found in PATH.")

    url = f"{base_url}/"
    with tempfile.TemporaryDirectory(prefix="coinbox-browser-", ignore_cleanup_errors=True) as user_data_dir:
        debug_port = _reserve_local_port()
        browser = subprocess.Popen(
            [
                chrome_binary,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--window-size=1280,900",
                f"--user-data-dir={user_data_dir}",
                f"--remote-debugging-port={debug_port}",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            version_url = f"http://127.0.0.1:{debug_port}/json/version"
            ready = _wait_until(
                lambda: _cdp_browser_ready(browser, version_url),
                timeout_s=5.0,
                poll_s=0.1,
            )
            assert ready, (
                "Headless Chrome DevTools endpoint did not start.\n"
                f"Browser stderr:\n{_read_process_stderr(browser)}"
            )

            target_info = _http_json(
                f"http://127.0.0.1:{debug_port}/json/new?{urllib.parse.quote(url, safe='')}",
                method="PUT",
            )
            ws_url = target_info.get("webSocketDebuggerUrl", "")
            assert ws_url, f"DevTools did not return a page websocket URL: {target_info}"

            sock = _ws_connect(ws_url)
            try:
                next_id = 1
                _cdp_send_command(sock, next_id, "Runtime.enable")
                next_id += 1

                ready_deadline = time.time() + HEADLESS_PAGE_INTERACTIVE_TIMEOUT_S
                last_state = {}
                while time.time() < ready_deadline:
                    state = _cdp_capture_page_state(sock, next_id)
                    next_id += 1
                    last_state = state
                    if state.get("current_path") == "/" and state.get("page_ready") == "1":
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError(f"Bootstrap page did not become interactive before click.\nLast state: {last_state}")

                click_response = _cdp_send_command(
                    sock,
                    next_id,
                    "Runtime.evaluate",
                    {
                        "expression": (
                            "(() => {"
                            "  const btn = document.getElementById('start-now');"
                            "  if (!btn) return 'missing-button';"
                            "  btn.click();"
                            "  return btn.textContent || '';"
                            "})()"
                        ),
                        "returnByValue": True,
                    },
                )
                next_id += 1
                click_value = click_response.get("result", {}).get("result", {}).get("value")
                assert click_value != "missing-button", "Bootstrap start-now button was not present in the browser DOM."

                deadline = time.time() + 8.0
                last_state = {
                    "current_url": url,
                    "current_path": "/",
                    "title": "",
                    "body_text": "",
                    "page_source": "",
                    "error": "",
                }
                while time.time() < deadline:
                    try:
                        state = _cdp_capture_page_state(sock, next_id)
                        next_id += 1
                        if state:
                            last_state = state
                            if state.get("current_path") in ("/sounds/", "/login") and state.get("page_ready") == "1":
                                break
                    except Exception as exc:
                        last_state["error"] = f"{type(exc).__name__}: {exc}"
                    time.sleep(0.05)

                diagnostics, next_id = _capture_settled_page_diagnostics(
                    sock,
                    next_id,
                    last_state,
                    settle_condition=lambda diagnostics: _page_load_diagnostics_ready(
                        diagnostics,
                        expected_path="/sounds/",
                        expected_script_paths=("/connection_monitor.js", "/navbar.js", "/glyphs.js"),
                        require_navbar=True,
                    ),
                    wait_s=8.0,
                )
                return diagnostics
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
        finally:
            if browser.poll() is None:
                browser.terminate()
                try:
                    browser.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    browser.kill()


def _normalize_resource_path(url_or_path: str) -> str:
    parsed = urllib.parse.urlparse(str(url_or_path))
    if parsed.scheme or parsed.netloc:
        return parsed.path or "/"
    return str(url_or_path)


def _first_contentful_paint_ms(browser_diagnostics: dict) -> float | None:
    for entry in browser_diagnostics.get("paint_timings", []):
        name = str(entry.get("name", "")).strip().lower()
        if name == "first-contentful-paint":
            try:
                return float(entry.get("startTime", 0.0))
            except (TypeError, ValueError):
                return None
    return None


def _browser_load_diagnostics_details(browser_diagnostics: dict, log_path) -> str:
    resource_lines = []
    for entry in browser_diagnostics.get("resource_timings", [])[:40]:
        resource_lines.append(
            f"{_normalize_resource_path(str(entry.get('name', '')))}"
            f" [{entry.get('initiatorType', '')}]"
            f" start={float(entry.get('startTime', 0.0)):.1f}"
            f" responseEnd={float(entry.get('responseEnd', 0.0)):.1f}"
        )

    paint_lines = []
    for entry in browser_diagnostics.get("paint_timings", []):
        paint_lines.append(
            f"{entry.get('name', '')}={float(entry.get('startTime', 0.0)):.1f}ms"
        )

    return (
        f"Browser URL: {browser_diagnostics.get('current_url')}\n"
        f"Browser title: {browser_diagnostics.get('title')}\n"
        f"Browser error: {browser_diagnostics.get('error')}\n"
        f"Page ready: {browser_diagnostics.get('page_ready')}\n"
        f"Script tags: {browser_diagnostics.get('script_paths', [])}\n"
        f"Navbar links: {browser_diagnostics.get('navbar_links', [])}\n"
        f"Connection monitor installed: {browser_diagnostics.get('has_connection_monitor_installed')}\n"
        f"Glyph API present: {browser_diagnostics.get('has_glyph_api')}\n"
        f"Glyph layer present: {browser_diagnostics.get('has_glyph_layer')}\n"
        f"Paint timings: {paint_lines}\n"
        f"Resource timings:\n" + "\n".join(resource_lines[:40]) + "\n"
        f"Body text:\n{browser_diagnostics.get('body_text', '')[:1200]}\n"
        f"DOM snippet:\n{browser_diagnostics.get('page_source', '')[:1200]}\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )


def _assert_js_dependencies_loaded_before_first_paint(
    browser_diagnostics: dict,
    expected_script_paths: tuple[str, ...],
    log_path,
):
    details = _browser_load_diagnostics_details(browser_diagnostics, log_path)
    first_contentful_paint_ms = _first_contentful_paint_ms(browser_diagnostics)
    assert first_contentful_paint_ms is not None, (
        "Could not read first-contentful-paint timing for the page under test.\n"
        f"{details}"
    )

    script_paths = {str(path) for path in browser_diagnostics.get("script_paths", [])}
    missing_script_tags = [path for path in expected_script_paths if path not in script_paths]
    assert not missing_script_tags, (
        "Expected external script tags were missing from the rendered page.\n"
        f"Missing: {missing_script_tags}\n"
        f"{details}"
    )

    late_or_missing_resources = []
    for script_path in expected_script_paths:
        matching_entries = [
            entry
            for entry in browser_diagnostics.get("resource_timings", [])
            if _normalize_resource_path(str(entry.get("name", ""))) == script_path
        ]
        if not matching_entries:
            late_or_missing_resources.append(f"{script_path} (no resource timing entry)")
            continue

        response_end_ms = max(float(entry.get("responseEnd", 0.0)) for entry in matching_entries)
        if response_end_ms <= 0.0:
            late_or_missing_resources.append(f"{script_path} (responseEnd={response_end_ms:.1f}ms)")
            continue
        if response_end_ms > first_contentful_paint_ms:
            late_or_missing_resources.append(
                f"{script_path} (responseEnd={response_end_ms:.1f}ms, fcp={first_contentful_paint_ms:.1f}ms)"
            )

    assert not late_or_missing_resources, (
        "The page became visible before all required JavaScript dependencies had finished loading.\n"
        f"Late or missing: {late_or_missing_resources}\n"
        f"{details}"
    )


# Test: Browser handoff from bootstrap should reach login when auth is already enabled.
# 1. Start main app and enable auth.
# 2. Restart back into bootstrap while auth remains enabled.
# 3. Open `/` in a real headless browser and click `Start now`.
# 4. Assert the browser leaves bootstrap and lands on `/login`.
def test_skip_handoff_browser_detects_login_when_auth_already_enabled(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _skip_to_main_app(base_url, log_path)
    auth_cookie = _set_security_password(base_url, CUSTOM_UI_PASSWORD)
    _restart_into_bootstrap(base_url, log_path, headers={"Cookie": auth_cookie})

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    states = _capture_start_now_transition_states(base_url)
    browser_state = next((state for state in reversed(states) if state.get("current_path") == "/login"), states[-1])
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/login",
        expected_title_fragment="login",
        log_path=log_path,
        message="Bootstrap handoff did not reach the login page after auth-enabled startup.",
    )


# Test: Browser handoff from bootstrap should reach the main app when auth is disabled.
# 1. Confirm bootstrap mode is active.
# 2. Open `/` in a real headless browser and click `Start now`.
# 3. Assert the browser leaves bootstrap and lands on `/sounds/`.
def test_skip_handoff_browser_reaches_main_app_without_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    states = _capture_start_now_transition_states(base_url)
    browser_state = next((state for state in reversed(states) if state.get("current_path") == "/sounds/"), states[-1])
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Bootstrap handoff did not reach the main app when auth was disabled.",
    )


# Test: Recovery page should show vendor, firmware, hardware, and MAC details.
# 1. Enter recovery mode.
# 2. Open the recovery page in a real headless browser.
# 3. Wait until the expected metadata strings are visible in the DOM.
def test_recovery_browser_shows_vendor_firmware_hardware_and_mac(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    expected_mac = _get_qemu_factory_mac()

    _enter_recovery_mode(base_url, log_path)

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/",
        wait_condition=lambda state: (
            "Vendor: TuDo Makerspace" in state.get("body_text", "")
            and _body_text_matches(state, r"Firmware:\s*\d+\.\d+\.\d+")
            and _body_text_matches(state, r"Hardware:\s*\d+\.\d+\.\d+")
            and _body_text_matches(state, rf"MAC:\s*{re.escape(expected_mac)}")
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    assert browser_state.get("current_path") == "/", details
    assert "Vendor: TuDo Makerspace" in browser_state.get("body_text", ""), details
    assert _body_text_matches(browser_state, r"Firmware:\s*\d+\.\d+\.\d+"), details
    assert _body_text_matches(browser_state, r"Hardware:\s*\d+\.\d+\.\d+"), details
    assert _body_text_matches(browser_state, rf"MAC:\s*{re.escape(expected_mac)}"), details


# Test: Bootstrap root should not become visible until its external JavaScript dependencies are loaded.
# 1. Open `/` in a real headless browser.
# 2. Wait until the bootstrap screen is visible and interactive.
# 3. Assert that all external script dependencies finished loading before first contentful paint.
# 4. Assert that the expected script side effects are already present in the DOM/runtime.
def test_bootstrap_browser_loads_javascript_dependencies_before_first_paint(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    browser_diagnostics = _capture_page_load_diagnostics_in_headless_chrome(
        f"{base_url}/",
        wait_condition=lambda state: (
            state.get("page_ready") == "1"
            and state.get("current_path") == "/"
            and "Coinbox is starting" in state.get("body_text", "")
            and "Start now" in state.get("body_text", "")
        ),
        settle_condition=lambda diagnostics: _page_load_diagnostics_ready(
            diagnostics,
            expected_path="/",
            expected_script_paths=("/connection_monitor.js", "/glyphs.js"),
        ),
        wait_s=10.0,
    )
    details = _browser_load_diagnostics_details(browser_diagnostics, log_path)
    _assert_browser_lands_on(
        browser_state=browser_diagnostics,
        expected_path="/",
        expected_title_fragment="starting",
        log_path=log_path,
        message="Bootstrap root page did not load in the browser as expected.",
    )
    _assert_js_dependencies_loaded_before_first_paint(
        browser_diagnostics=browser_diagnostics,
        expected_script_paths=("/connection_monitor.js", "/glyphs.js"),
        log_path=log_path,
    )
    assert browser_diagnostics.get("has_connection_monitor_installed") is True, details
    assert browser_diagnostics.get("has_glyph_api") is True, details
    assert browser_diagnostics.get("has_glyph_layer") is True, details


# Test: Exiting recovery mode should immediately show the intended handoff card.
# 1. Enter recovery mode.
# 2. Open the recovery page in a real headless browser.
# 3. Click the "Exit recovery mode" button.
# 4. Assert the first non-recovery handoff screen already shows the "Getting things ready..." card,
#    without the countdown or recovery/start buttons.
def test_recovery_exit_browser_immediately_shows_starting_main_application(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    _enter_recovery_mode(base_url, log_path)

    states = _capture_recovery_exit_transition_states(base_url)
    details = _transition_states_details(states, log_path)
    assert states, f"No browser states were captured after exiting recovery mode.\n{details}"

    transition_state = next(
        (
            state for state in states
            if "Recovery Mode" not in state.get("body_text", "")
            and (
                "Starting main application" in state.get("body_text", "")
                or "Getting things ready..." in state.get("body_text", "")
                or "Coinbox is starting" in state.get("body_text", "")
            )
        ),
        None,
    )
    assert transition_state is not None, (
        "Did not observe a bootstrap handoff screen after clicking exit recovery mode.\n"
        f"{details}"
    )
    assert "Starting main application" in transition_state.get("body_text", ""), (
        "The first post-recovery handoff screen was not 'Starting main application'.\n"
        f"{details}"
    )
    assert "Getting things ready..." in transition_state.get("body_text", ""), (
        "The first post-recovery handoff screen did not show the intended handoff card body.\n"
        f"{details}"
    )
    assert "Coinbox is starting" not in transition_state.get("body_text", ""), (
        "Observed an intermediate 'Coinbox is starting' screen after clicking exit recovery mode.\n"
        f"{details}"
    )
    assert "seconds remaining" not in transition_state.get("body_text", ""), (
        "Observed the countdown on the first post-recovery handoff screen.\n"
        f"{details}"
    )
    assert "Start now" not in transition_state.get("body_text", ""), (
        "Observed the Start now button on the first post-recovery handoff screen.\n"
        f"{details}"
    )
    assert "Enter recovery mode" not in transition_state.get("body_text", ""), (
        "Observed the recovery-entry button on the first post-recovery handoff screen.\n"
        f"{details}"
    )
    assert "Exit recovery mode" not in transition_state.get("body_text", ""), (
        "Observed the recovery-exit button on the first post-recovery handoff screen.\n"
        f"{details}"
    )
    assert any(state.get("current_path") == "/sounds/" for state in states), (
        "Browser did not complete the handoff into the main app after exiting recovery mode.\n"
        f"{details}"
    )


# Test: Browser countdown expiry should hand off to the main app when auth is disabled.
# 1. Confirm bootstrap mode is active.
# 2. Wait until the countdown is nearly expired to keep the test fast.
# 3. Open `/` in a real headless browser and let the countdown hit zero.
# 4. Assert the browser auto-handoff lands on `/sounds/`.
def test_expire_browser_reaches_main_app_without_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _wait_for_bootstrap_countdown_threshold(
        base_url=base_url,
        threshold_s=2,
        timeout_s=BOOT_TIMEOUT_S,
    )
    assert countdown_s is not None, (
        "Could not reach late-countdown browser handoff window before bootstrap ended.\n"
        "Threshold: <= 2s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/",
        wait_paths=("/sounds/",),
        wait_s=10.0,
    )
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Countdown expiry did not navigate the browser into the main app without auth.",
    )


# Test: Browser countdown expiry should hand off to login when auth is already enabled.
# 1. Start main app and enable auth.
# 2. Restart back into bootstrap while auth remains enabled.
# 3. Wait until the countdown is nearly expired to keep the test fast.
# 4. Open `/` in a real headless browser and let the countdown hit zero.
# 5. Assert the browser auto-handoff lands on `/login`.
def test_expire_browser_reaches_login_when_auth_already_enabled(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    _skip_to_main_app(base_url, log_path)
    auth_cookie = _set_security_password(base_url, CUSTOM_UI_PASSWORD)
    _restart_into_bootstrap(base_url, log_path, headers={"Cookie": auth_cookie})

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    countdown_s = _wait_for_bootstrap_countdown_threshold(
        base_url=base_url,
        threshold_s=2,
        timeout_s=BOOT_TIMEOUT_S,
    )
    assert countdown_s is not None, (
        "Could not reach late-countdown browser handoff window before bootstrap ended.\n"
        "Threshold: <= 2s\n"
        f"Log tail:\n{_tail_log(log_path)}"
    )

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/",
        wait_paths=("/login",),
        wait_s=15.0,
    )
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/login",
        expected_title_fragment="login",
        log_path=log_path,
        message="Countdown expiry did not navigate the browser to login when auth was enabled.",
    )


# Test: The first main-app page reached from bootstrap should not become visible
# until its external JavaScript dependencies are loaded.
# 1. Start from bootstrap mode with auth disabled.
# 2. Open `/` in a real headless browser and click `Start now`.
# 3. Wait until the browser reaches `/sounds/`.
# 4. Assert that all external script dependencies finished loading before first contentful paint.
# 5. Assert that the navbar, connection monitor, and glyph helper are already active.
def test_mainapp_entry_browser_loads_javascript_dependencies_before_first_paint(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    browser_diagnostics = _capture_start_now_load_diagnostics(base_url)
    details = _browser_load_diagnostics_details(browser_diagnostics, log_path)
    _assert_browser_lands_on(
        browser_state=browser_diagnostics,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Bootstrap handoff did not reach the Sounds page before JavaScript dependency checks ran.",
    )
    _assert_js_dependencies_loaded_before_first_paint(
        browser_diagnostics=browser_diagnostics,
        expected_script_paths=("/connection_monitor.js", "/navbar.js", "/glyphs.js"),
        log_path=log_path,
    )
    assert browser_diagnostics.get("has_connection_monitor_installed") is True, details
    assert browser_diagnostics.get("has_glyph_api") is True, details
    assert browser_diagnostics.get("has_glyph_layer") is True, details
    assert browser_diagnostics.get("has_navbar_style") is True, details
    assert browser_diagnostics.get("navbar_links") == ["Sounds", "Settings"], details


# Test: Settings System card should show firmware, hardware, vendor, recovery code, source, and license information.
# 1. Start the main app.
# 2. Open `/settings` in a real headless browser.
# 3. Wait until the expected System card text is visible in the DOM.
def test_settings_browser_shows_system_card_metadata(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    expected_recovery_code = _expected_recovery_code()

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/settings",
        wait_condition=lambda state: (
            "Vendor: TuDo Makerspace" in state.get("body_text", "")
            and _body_text_matches(state, r"Firmware:\s*\d+\.\d+\.\d+")
            and _body_text_matches(state, r"Hardware:\s*\d+\.\d+\.\d+")
            and _body_text_matches(state, rf"Recovery Code:\s*{expected_recovery_code:04d}")
            and "Source Code: https://github.com/TuDo-Makerspace/Coinbox" in state.get("body_text", "")
            and "License: MIT License" in state.get("body_text", "")
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/settings",
        expected_title_fragment="settings",
        log_path=log_path,
        message="Settings page did not render the expected System card metadata.",
    )
    assert "Vendor: TuDo Makerspace" in browser_state.get("body_text", ""), details
    assert _body_text_matches(browser_state, r"Firmware:\s*\d+\.\d+\.\d+"), details
    assert _body_text_matches(browser_state, r"Hardware:\s*\d+\.\d+\.\d+"), details
    assert _body_text_matches(browser_state, rf"Recovery Code:\s*{expected_recovery_code:04d}"), details
    assert "Source Code: https://github.com/TuDo-Makerspace/Coinbox" in browser_state.get("body_text", ""), details
    assert "License: MIT License" in browser_state.get("body_text", ""), details


# Test: Settings Network card should show the device MAC address.
# 1. Start the main app.
# 2. Open `/settings` in a real headless browser.
# 3. Wait until the expected MAC string is visible in the DOM.
def test_settings_browser_shows_network_mac(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    expected_mac = _get_qemu_factory_mac()

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/settings",
        wait_condition=lambda state: _body_text_matches(
            state,
            rf"\b{re.escape(expected_mac)}\b",
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/settings",
        expected_title_fragment="settings",
        log_path=log_path,
        message="Settings page did not render the expected Network card MAC address.",
    )
    assert _body_text_matches(browser_state, rf"\b{re.escape(expected_mac)}\b"), details


# Test: Sounds page shows a temporary heads-up when a 0%-volume sound is selected for playback.
# 1. Start the main app and upload a dedicated MP3 fixture row.
# 2. Set that row's volume to `0` while keeping at least one other sound non-zero to avoid the global warning note.
# 3. Trigger playback either manually from the row play button or via laser injection.
# 4. Assert the transient 0%-volume playback notice appears in the rendered DOM.
# 5. Assert the notice disappears again after a short while.
@pytest.mark.parametrize("trigger_mode", ["manual", "laser"])
def test_sounds_browser_shows_and_clears_zero_volume_playback_notice(
    qemu_mainapp_instance,
    trigger_mode: str,
):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    filename = f"browser-zero-volume-{trigger_mode}.mp3"
    notice_pattern = r"(?:volume.*0%.*(?:skip|skipped|not be played|won't play|will not play)|(?:skip|skipped).*volume.*0%)"

    _upload_sound_fixture(base_url, filename)
    _set_sound_meta(base_url, filename, {"enabled": True, "probability": 100, "volume": 0})

    trigger_expression = None
    after_ready = None
    if trigger_mode == "manual":
        _set_sound_meta(base_url, DEFAULT_SOUND_FILENAME, {"enabled": True, "probability": 100, "volume": 100})
        trigger_expression = (
            "((targetName) => {"
            "  const row = Array.from(document.querySelectorAll('.file-item'))"
            "    .find((item) => item.dataset && item.dataset.fileName === targetName);"
            "  if (!row) return 'missing-row';"
            "  const btn = row.querySelector('button[data-k=\"play\"]');"
            "  if (!btn) return 'missing-play-button';"
            "  btn.click();"
            "  return targetName;"
            f"}})({json.dumps(filename)})"
        )
    else:
        _set_sound_meta(base_url, DEFAULT_SOUND_FILENAME, {"enabled": True, "probability": 0, "volume": 100})

        def after_ready():
            _trigger_test_laser_playback(base_url)

    states = _capture_sounds_notice_transition_states(
        base_url,
        ready_row_name=filename,
        trigger_expression=trigger_expression,
        after_ready=after_ready,
        wait_s=6.0,
    )
    assert states, f"Expected browser states for zero-volume playback notice test ({trigger_mode})."

    appeared_idx = _first_matching_state_index(states, notice_pattern)
    cleared_idx = None
    if appeared_idx is not None:
        for idx in range(appeared_idx + 1, len(states)):
            if not _body_text_matches(states[idx], notice_pattern):
                cleared_idx = idx
                break

    details = (
        _browser_state_details(states[-1], log_path)
        + f"\nTrigger mode: {trigger_mode}"
        + f"\nNotice present flags: {[bool(_body_text_matches(state, notice_pattern)) for state in states]}"
    )
    assert not _body_text_matches(states[0], notice_pattern), details
    assert appeared_idx is not None, (
        f"Sounds page did not show the transient zero-volume playback notice for {trigger_mode} trigger.\n{details}"
    )
    assert cleared_idx is not None, (
        f"Sounds page did not clear the transient zero-volume playback notice for {trigger_mode} trigger.\n{details}"
    )


# Test: Sounds page shows a warning note when lid-closed volume is configured to 0%.
# 1. Start the main app and force lid-closed state through the hall test GPIO.
# 2. Set `lid_closed_volume_pct=0` while keeping lid-open volume non-zero.
# 3. Open `/sounds/` in a real headless browser.
# 4. Wait until the warning note text appears in the rendered DOM.
def test_sounds_browser_warns_when_lid_closed_volume_is_zero(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    audio_cfg = _set_audio_config(
        base_url,
        {"lid_closed_volume_pct": 0, "lid_open_volume_pct": 25},
    )
    assert audio_cfg.get("lid_closed_volume_pct") == 0
    assert audio_cfg.get("lid_open_volume_pct") == 25

    _set_test_gpio_level(base_url, "hall", 0)

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/sounds/",
        wait_condition=lambda state: (
            state.get("current_path") == "/sounds/"
            and _body_text_matches(state, r"lid\s*closed\s*volume")
            and _body_text_matches(state, r"\b0%")
            and _body_text_matches(state, r"muted|lid\s+is\s+closed")
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Sounds page did not render the lid-closed zero-volume warning note.",
    )
    assert _body_text_matches(browser_state, r"lid\s*closed\s*volume"), details
    assert _body_text_matches(browser_state, r"\b0%"), details
    assert _body_text_matches(browser_state, r"muted|lid\s+is\s+closed"), details


# Test: Sounds page shows a warning note when no sounds are enabled.
# 1. Start the main app and ensure only the built-in default sound is present.
# 2. Disable that sound so no enabled sounds remain.
# 3. Open `/sounds/` in a real headless browser.
# 4. Wait until the no-enabled-sounds warning appears in the rendered DOM.
def test_sounds_browser_warns_when_no_sounds_are_enabled(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    _set_sound_meta(base_url, "default.mp3", {"enabled": False, "probability": 100, "volume": 100})

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/sounds/",
        wait_condition=lambda state: (
            state.get("current_path") == "/sounds/"
            and _body_text_matches(state, r"all\s+sounds?.*disabled|no\s+sound.*played\s+on\s+coin\s+insertion")
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Sounds page did not render the no-enabled-sounds warning note.",
    )
    assert _body_text_matches(
        browser_state,
        r"all\s+sounds?.*disabled|no\s+sound.*played\s+on\s+coin\s+insertion",
    ), details


# Test: Sounds page shows a warning note when all sounds have zero weight.
# 1. Start the main app and ensure only the built-in default sound is present.
# 2. Set that sound's weight to `0`.
# 3. Open `/sounds/` in a real headless browser.
# 4. Wait until the zero-weight warning appears in the rendered DOM.
def test_sounds_browser_warns_when_all_sounds_have_no_weight(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    _set_sound_meta(base_url, "default.mp3", {"enabled": True, "probability": 0, "volume": 100})

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/sounds/",
        wait_condition=lambda state: (
            state.get("current_path") == "/sounds/"
            and _body_text_matches(state, r"all\s+sounds?.*weight|no\s+sound.*weight|weight.*0")
            and _body_text_matches(state, r"no\s+sound.*played\s+on\s+coin\s+insertion")
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Sounds page did not render the zero-weight warning note.",
    )
    assert _body_text_matches(
        browser_state,
        r"all\s+sounds?.*weight|no\s+sound.*weight|weight.*0",
    ), details
    assert _body_text_matches(browser_state, r"no\s+sound.*played\s+on\s+coin\s+insertion"), details


# Test: Sounds page shows a warning note when all sounds have volume 0%.
# 1. Start the main app and ensure only the built-in default sound is present.
# 2. Set that sound's volume to `0` while keeping it enabled and weighted.
# 3. Open `/sounds/` in a real headless browser.
# 4. Wait until the zero-volume warning appears in the rendered DOM.
def test_sounds_browser_warns_when_all_sounds_have_zero_volume(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    _set_sound_meta(base_url, "default.mp3", {"enabled": True, "probability": 100, "volume": 0})

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/sounds/",
        wait_condition=lambda state: (
            state.get("current_path") == "/sounds/"
            and _body_text_matches(state, r"all\s+sounds?.*volume|no\s+sound.*volume|volume.*0%")
            and _body_text_matches(state, r"no\s+sound.*played\s+on\s+coin\s+insertion")
        ),
    )
    details = _browser_state_details(browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Sounds page did not render the zero-volume warning note.",
    )
    assert _body_text_matches(
        browser_state,
        r"all\s+sounds?.*volume|no\s+sound.*volume|volume.*0%",
    ), details
    assert _body_text_matches(browser_state, r"no\s+sound.*played\s+on\s+coin\s+insertion"), details


# Test: The shared runtime-status boot ID and page-root boot ID should track the active boot instance.
# 1. Read `/runtime/status` in the running main app and capture its boot ID.
# 2. Open `/settings` in a browser and assert the page root exposes the same boot ID.
# 3. Restart the device and wait until bootstrap is back.
# 4. Assert `/runtime/status` now reports a different boot ID.
# 5. Open `/` again in a browser and assert the page root exposes that new boot ID.
def test_runtime_status_and_page_root_boot_id_change_after_restart(qemu_mainapp_instance):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    initial_status = _get_runtime_status(base_url)
    initial_boot_id = str(initial_status.get("boot_id", ""))
    assert initial_boot_id, f"Missing initial boot_id from /runtime/status: {initial_status!r}"

    initial_browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/settings",
        wait_condition=lambda state: (
            state.get("current_path") == "/settings"
            and state.get("page_ready") == "1"
            and state.get("page_boot_id") == initial_boot_id
        ),
    )
    initial_details = _browser_state_details(initial_browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=initial_browser_state,
        expected_path="/settings",
        expected_title_fragment="settings",
        log_path=log_path,
        message="Settings page did not load before validating the initial page-root boot ID.",
    )
    assert initial_browser_state.get("page_boot_id") == initial_boot_id, (
        "The settings page root boot ID did not match /runtime/status before restart.\n"
        f"{initial_details}"
    )

    _restart_into_bootstrap(base_url, log_path)

    rebooted_status = _get_runtime_status(base_url)
    rebooted_boot_id = str(rebooted_status.get("boot_id", ""))
    assert rebooted_boot_id, f"Missing rebooted boot_id from /runtime/status: {rebooted_status!r}"
    assert rebooted_boot_id != initial_boot_id, (
        "Expected /runtime/status boot_id to change after restart.\n"
        f"before={initial_boot_id!r} after={rebooted_boot_id!r}"
    )

    rebooted_browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/",
        wait_condition=lambda state: (
            state.get("current_path") == "/"
            and state.get("page_ready") == "1"
            and state.get("page_boot_id") == rebooted_boot_id
        ),
    )
    rebooted_details = _browser_state_details(rebooted_browser_state, log_path)
    _assert_browser_lands_on(
        browser_state=rebooted_browser_state,
        expected_path="/",
        expected_title_fragment="starting",
        log_path=log_path,
        message="Bootstrap root page did not load before validating the rebooted page-root boot ID.",
    )
    assert rebooted_browser_state.get("page_boot_id") == rebooted_boot_id, (
        "The bootstrap root page boot ID did not match /runtime/status after restart.\n"
        f"{rebooted_details}"
    )


# Test: Settings actions that reboot the device should immediately show the connection-lost popup.
# 1. Start in the main app on `/settings`.
# 2. Trigger one of the settings-page reboot actions.
# 3. Assert the browser shows the built-in "Connection lost!" overlay on `/settings`
#    without waiting for a manual refresh or a different page.
@pytest.mark.parametrize(
    ("action_button_id", "action_label"),
    [
        ("restart-device", "restart device"),
        ("reset-settings", "reset settings"),
    ],
)
def test_settings_restart_actions_browser_immediately_show_connection_lost_popup(
    qemu_mainapp_instance,
    action_button_id: str,
    action_label: str,
):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]

    states = _capture_settings_restart_action_states(base_url, action_button_id, log_path)
    details = _timed_transition_states_details(states, log_path)
    assert states, f"No browser states were captured after triggering {action_label} from settings.\n{details}"

    overlay_state = next((state for state in states if state.get("connection_lost_visible") is True), None)
    assert overlay_state is not None, (
        f"The browser never showed the connection-lost overlay after triggering {action_label} from settings.\n"
        f"{details}"
    )
    assert overlay_state.get("current_path") == "/settings", (
        f"The connection-lost overlay for {action_label} did not appear while still on the settings page.\n"
        f"{details}"
    )
    assert float(overlay_state.get("elapsed_s", 999.0)) <= 2.0, (
        f"The connection-lost overlay was not shown immediately after triggering {action_label} from settings.\n"
        f"{details}"
    )
    assert "Connection lost!" in overlay_state.get("body_text", ""), (
        f"The expected overlay text was not visible after triggering {action_label} from settings.\n"
        f"{details}"
    )
    initial_boot_id = str(states[0].get("initial_boot_id", "")) if states else ""
    assert initial_boot_id, (
        f"Did not capture the initial boot_id before triggering {action_label} from settings.\n"
        f"{details}"
    )
    boot_id_change_state = next((state for state in states if state.get("new_boot_id_seen") is True), None)
    assert boot_id_change_state is not None, (
        f"Did not observe /runtime/status report a new boot_id after triggering {action_label} from settings.\n"
        f"{details}"
    )
    reboot_state = next((state for state in states if state.get("reboot_seen") is True), None)
    assert reboot_state is not None, (
        f"Did not observe a reboot marker in the QEMU log after triggering {action_label} from settings.\n"
        f"{details}"
    )
    assert not any(
        state.get("current_path") != "/settings" for state in states[: states.index(overlay_state) + 1]
    ), (
        f"The browser left /settings before showing the connection-lost overlay for {action_label}.\n"
        f"{details}"
    )
    overlay_index = states.index(overlay_state)
    boot_id_change_index = states.index(boot_id_change_state)
    assert not any(
        state.get("connection_lost_visible") is not True for state in states[overlay_index:boot_id_change_index]
    ), (
        f"The connection-lost overlay did not stay visible until a different boot instance became reachable for {action_label}.\n"
        f"{details}"
    )
    reconnect_state = next((state for state in states if state.get("site_reconnected") is True), None)
    assert reconnect_state is not None, (
        f"The browser did not automatically reconnect after triggering {action_label} from settings.\n"
        f"{details}"
    )
    reconnect_index = states.index(reconnect_state)
    reboot_index = states.index(reboot_state)
    assert reconnect_index >= boot_id_change_index, (
        f"The browser reported reconnection before /runtime/status exposed a new boot_id for {action_label}.\n"
        f"{details}"
    )
    assert reconnect_index >= reboot_index, (
        f"The browser reported reconnection before the reboot was observed for {action_label}.\n"
        f"{details}"
    )
    assert reconnect_state.get("page_boot_id") == reconnect_state.get("runtime_boot_id"), (
        f"The browser reconnected to a page whose root boot_id did not match /runtime/status for {action_label}.\n"
        f"{details}"
    )
    assert reconnect_state.get("page_boot_id") != initial_boot_id, (
        f"The browser reconnected without ever leaving the old boot instance for {action_label}.\n"
        f"{details}"
    )
    assert float(reconnect_state.get("elapsed_s", 999.0)) - float(boot_id_change_state.get("elapsed_s", 0.0)) <= 1.0, (
        f"The browser did not reconnect soon after a new boot_id became reachable for {action_label}.\n"
        f"{details}"
    )
    assert reconnect_state.get("connection_lost_visible") is not True, (
        f"The connection-lost overlay did not clear after the new boot instance became available for {action_label}.\n"
        f"{details}"
    )
    ready_state = next((state for state in states if state.get("site_ready") is True), None)
    assert ready_state is not None, (
        f"The browser never settled on a ready page after reconnecting for {action_label}.\n"
        f"{details}"
    )
    assert ready_state.get("page_ready") == "1", (
        f"The browser did not eventually reach a ready page after reconnecting for {action_label}.\n"
        f"{details}"
    )


# Test: The handoff card should also show the connection-lost popup if the device disappears mid-handoff.
# 1. Start either from the normal bootstrap screen or from recovery mode.
# 2. Click the handoff button so the page shows "Starting main application / Getting things ready...".
# 3. Stop the emulator while that handoff card is visible.
# 4. Assert the built-in connection-lost overlay appears on top of that handoff card.
@pytest.mark.parametrize(
    ("recovery_mode", "action_label"),
    [
        (False, "bootstrap start-now handoff"),
        (True, "recovery exit handoff"),
    ],
)
def test_handoff_browser_shows_connection_lost_popup_after_disconnect(
    qemu_bootstrap_instance,
    recovery_mode: bool,
    action_label: str,
):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    proc = qemu_bootstrap_instance["process"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)
    if recovery_mode:
        _enter_recovery_mode(base_url, log_path)

    states = _capture_handoff_disconnect_states(base_url, proc, recovery_mode)
    details = _timed_transition_states_details(states, log_path)
    assert states, f"No browser states were captured while exercising the {action_label}.\n{details}"

    handoff_state = next(
        (
            state for state in states
            if state.get("current_path") == "/"
            and "Starting main application" in state.get("body_text", "")
            and "Getting things ready..." in state.get("body_text", "")
        ),
        None,
    )
    assert handoff_state is not None, (
        f"Did not observe the intended handoff card before disconnecting during the {action_label}.\n"
        f"{details}"
    )

    overlay_state = next((state for state in states if state.get("connection_lost_visible") is True), None)
    assert overlay_state is not None, (
        f"The browser never showed the connection-lost overlay during the {action_label}.\n"
        f"{details}"
    )
    assert overlay_state.get("current_path") == "/", (
        f"The browser left the handoff page before showing the connection-lost overlay during the {action_label}.\n"
        f"{details}"
    )
    assert "Connection lost!" in overlay_state.get("body_text", ""), (
        f"The expected overlay text was not visible during the {action_label}.\n"
        f"{details}"
    )
    assert "Starting main application" in overlay_state.get("body_text", ""), (
        f"The underlying handoff title was not still visible when the overlay appeared during the {action_label}.\n"
        f"{details}"
    )
    assert "Getting things ready..." in overlay_state.get("body_text", ""), (
        f"The underlying handoff body was not still visible when the overlay appeared during the {action_label}.\n"
        f"{details}"
    )


# Test: If the device disappears during handoff, the browser should keep showing the
# "Starting main application / Getting things ready..." card until the configured
# handoff disconnect-watch delay expires, instead of falling through to Chrome's
# native connection error page.
@pytest.mark.parametrize(
    ("recovery_mode", "action_label"),
    [
        (False, "bootstrap start-now handoff"),
        (True, "recovery exit handoff"),
    ],
)
def test_handoff_browser_keeps_handoff_card_visible_until_disconnect_watch_delay_expires(
    qemu_bootstrap_instance,
    recovery_mode: bool,
    action_label: str,
):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    proc = qemu_bootstrap_instance["process"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)
    if recovery_mode:
        _enter_recovery_mode(base_url, log_path)
        status, headers, body = _http_get(base_url, "/")
        assert _is_bootstrap_root_page(status, headers, body)

    handoff_delay_ms = _extract_main_handoff_connection_monitor_delay_ms(body)
    assert handoff_delay_ms is not None, (
        "Bootstrap page did not expose MAIN_HANDOFF_CONNECTION_MONITOR_DELAY_MS.\n"
        f"Body:\n{body[:1200]}"
    )
    handoff_delay_s = handoff_delay_ms / 1000.0

    states = _capture_handoff_disconnect_states(base_url, proc, recovery_mode)
    details = _timed_transition_states_details(states, log_path)
    assert states, f"No browser states were captured while exercising the {action_label}.\n{details}"

    handoff_state = next((state for state in states if _state_shows_handoff_card(state)), None)
    assert handoff_state is not None, (
        f"Did not observe the intended handoff card before disconnecting during the {action_label}.\n"
        f"{details}"
    )
    overlay_state = next((state for state in states if state.get("connection_lost_visible") is True), None)
    assert overlay_state is not None, (
        f"The browser never showed the connection-lost overlay during the {action_label}.\n"
        f"{details}"
    )

    handoff_index = states.index(handoff_state)
    overlay_index = states.index(overlay_state)
    assert overlay_index >= handoff_index, (
        f"The connection-lost overlay appeared before the handoff card was observed during the {action_label}.\n"
        f"{details}"
    )

    pre_overlay_states = states[handoff_index:overlay_index]
    assert pre_overlay_states, (
        f"The browser showed the connection-lost overlay immediately after entering handoff during the {action_label}.\n"
        f"{details}"
    )
    assert not any(_state_shows_native_connection_error(state) for state in states[handoff_index : overlay_index + 1]), (
        f"The browser fell through to Chrome's native connection error page during the {action_label}.\n"
        f"{details}"
    )

    must_hold_until_s = float(handoff_state.get("elapsed_s", 0.0)) + max(0.0, handoff_delay_s - HANDOFF_DELAY_TOLERANCE_S)
    sustained_handoff_states = [
        state
        for state in pre_overlay_states
        if float(state.get("elapsed_s", 0.0)) <= must_hold_until_s
    ]
    assert sustained_handoff_states, (
        f"Did not capture any handoff-card states covering the configured disconnect-watch delay during the {action_label}.\n"
        f"expected_delay_s={handoff_delay_s:.2f}\n{details}"
    )
    assert all(_state_shows_handoff_card(state) for state in sustained_handoff_states), (
        f"The handoff card did not stay visible throughout the configured disconnect-watch delay during the {action_label}.\n"
        f"expected_delay_s={handoff_delay_s:.2f}\n{details}"
    )
    assert all(state.get("connection_lost_visible") is not True for state in sustained_handoff_states), (
        f"The connection-lost overlay appeared before the configured disconnect-watch delay expired during the {action_label}.\n"
        f"expected_delay_s={handoff_delay_s:.2f}\n{details}"
    )

    overlay_delay_s = float(overlay_state.get("elapsed_s", 0.0)) - float(handoff_state.get("elapsed_s", 0.0))
    assert overlay_delay_s >= max(0.0, handoff_delay_s - HANDOFF_DELAY_TOLERANCE_S), (
        f"The connection-lost overlay appeared before the configured disconnect-watch delay expired during the {action_label}.\n"
        f"expected_delay_s={handoff_delay_s:.2f} actual_delay_s={overlay_delay_s:.2f}\n"
        f"{details}"
    )


# Test: Bootstrap root page should show the connection-lost popup after the device disappears.
# 1. Open `/` in a real headless browser and confirm bootstrap is loaded.
# 2. Stop the QEMU process to simulate the device going away.
# 3. Wait for the page's periodic fetches to fail twice.
# 4. Assert the connection-lost overlay becomes visible.
def test_bootstrap_browser_shows_connection_lost_popup_after_disconnect(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]
    proc = qemu_bootstrap_instance["process"]
    disconnect_triggered = {"done": False}

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/",
        wait_condition=lambda state: _wait_for_connection_lost_popup_after_disconnect(
            state,
            proc,
            disconnect_triggered,
            "/",
            "starting",
        ),
        wait_s=10.0,
    )
    _assert_connection_lost_popup_visible(
        browser_state=browser_state,
        disconnect_triggered=disconnect_triggered,
        log_path=log_path,
        message="Bootstrap page did not show the connection-lost popup after the device disappeared.",
    )


@pytest.mark.parametrize(
    ("page_path", "title_fragment"),
    [
        ("/sounds/", "sounds"),
        ("/settings", "settings"),
    ],
)
def test_mainapp_browser_shows_connection_lost_popup_after_disconnect(
    qemu_mainapp_instance,
    page_path: str,
    title_fragment: str,
):
    base_url = qemu_mainapp_instance["base_url"]
    log_path = qemu_mainapp_instance["log_path"]
    proc = qemu_mainapp_instance["process"]
    disconnect_triggered = {"done": False}

    status, _, _ = _http_get(base_url, page_path)
    assert status == 200, f"Expected {page_path} to be reachable before browser disconnect test, got {status}"

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}{page_path}",
        wait_condition=lambda state: _wait_for_connection_lost_popup_after_disconnect(
            state,
            proc,
            disconnect_triggered,
            page_path,
            title_fragment,
        ),
        wait_s=10.0,
    )
    _assert_connection_lost_popup_visible(
        browser_state=browser_state,
        disconnect_triggered=disconnect_triggered,
        log_path=log_path,
        message=f"{page_path} did not show the connection-lost popup after the device disappeared.",
    )
