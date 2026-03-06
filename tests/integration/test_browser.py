from __future__ import annotations

import base64
import hashlib
import json
import os
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
        _http_get,
        _is_bootstrap_root_page,
        _restart_into_bootstrap,
        _set_security_password,
        _skip_to_main_app,
        _stop_process_group,
        _tail_log,
        _wait_for_bootstrap_countdown_threshold,
        _wait_until,
        qemu_bootstrap_instance,
    )
except ModuleNotFoundError:
    from integration_helpers import (
        BOOT_TIMEOUT_S,
        CUSTOM_UI_PASSWORD,
        _http_get,
        _is_bootstrap_root_page,
        _restart_into_bootstrap,
        _set_security_password,
        _skip_to_main_app,
        _stop_process_group,
        _tail_log,
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


def _capture_browser_state_in_headless_chrome(
    url: str,
    wait_paths: tuple[str, ...] = (),
    wait_condition=None,
    wait_s: float = 8.0,
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
                            if wait_paths and state.get("current_path") in wait_paths:
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


def _browser_state_details(browser_state: dict, log_path) -> str:
    return (
        f"Browser URL: {browser_state.get('current_url')}\n"
        f"Browser title: {browser_state.get('title')}\n"
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


def _find_browser_binary() -> str | None:
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
        "has_connection_lost_overlay": bool(value.get("hasConnectionLostOverlay", False)),
        "connection_lost_visible": bool(value.get("connectionLostVisible", False)),
        "error": "",
    }


# Test: Browser handoff from bootstrap should reach login when auth is already enabled.
# 1. Start main app and enable auth.
# 2. Restart back into bootstrap while auth remains enabled.
# 3. Open `/skip` in a real headless browser and let the bootstrap handoff JS run.
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

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/skip",
        wait_paths=("/login",),
    )
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/login",
        expected_title_fragment="login",
        log_path=log_path,
        message="Bootstrap handoff did not reach the login page after auth-enabled startup.",
    )


# Test: Browser handoff from bootstrap should reach the main app when auth is disabled.
# 1. Confirm bootstrap mode is active.
# 2. Open `/skip` in a real headless browser and let the bootstrap handoff JS run.
# 3. Assert the browser leaves bootstrap and lands on `/sounds/`.
def test_skip_handoff_browser_reaches_main_app_without_auth(qemu_bootstrap_instance):
    base_url = qemu_bootstrap_instance["base_url"]
    log_path = qemu_bootstrap_instance["log_path"]

    status, headers, body = _http_get(base_url, "/")
    assert _is_bootstrap_root_page(status, headers, body)

    browser_state = _capture_browser_state_in_headless_chrome(
        f"{base_url}/skip",
        wait_paths=("/sounds/",),
    )
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/sounds/",
        expected_title_fragment="sounds",
        log_path=log_path,
        message="Bootstrap handoff did not reach the main app when auth was disabled.",
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
        wait_s=10.0,
    )
    _assert_browser_lands_on(
        browser_state=browser_state,
        expected_path="/login",
        expected_title_fragment="login",
        log_path=log_path,
        message="Countdown expiry did not navigate the browser to login when auth was enabled.",
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
