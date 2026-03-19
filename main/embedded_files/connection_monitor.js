(function () {
  if (window.__coinboxConnectionMonitorInstalled || typeof window.fetch !== "function") {
    return;
  }
  window.__coinboxConnectionMonitorInstalled = true;

  const OVERLAY_ID = "coinbox-connection-lost-overlay";
  const STYLE_ID = "coinbox-connection-style";
  const FAILURE_THRESHOLD = 2;
  const RECONNECT_POLL_MS = 200;
  const RECONNECT_REQUEST_TIMEOUT_MS = 1500;
  const RUNTIME_STATUS_PATH = "/runtime/status";
  const nativeFetch = window.fetch.bind(window);
  const FETCH_ALLOW_DURING_EXPECTED_DISCONNECT = "coinboxAllowDuringExpectedDisconnect";
  const DISCONNECT_WATCH_DEFAULT_POLL_MS = 500;
  let consecutiveFailures = 0;
  let overlay = null;
  let expectedDisconnect = false;
  let expectedDisconnectBootId = "";
  let reconnectPollTimer = null;
  let reconnectPollInFlight = false;
  let reconnectNavigationPending = false;
  let disconnectWatchDelayTimer = null;
  let disconnectWatchPollTimer = null;
  let disconnectWatchInFlight = false;

  function clearReconnectPolling() {
    if (reconnectPollTimer !== null) {
      window.clearInterval(reconnectPollTimer);
      reconnectPollTimer = null;
    }
    reconnectPollInFlight = false;
    expectedDisconnectBootId = "";
    reconnectNavigationPending = false;
  }

  function clearDisconnectWatch() {
    if (disconnectWatchDelayTimer !== null) {
      window.clearTimeout(disconnectWatchDelayTimer);
      disconnectWatchDelayTimer = null;
    }
    if (disconnectWatchPollTimer !== null) {
      window.clearInterval(disconnectWatchPollTimer);
      disconnectWatchPollTimer = null;
    }
    disconnectWatchInFlight = false;
  }

  function buildRuntimeStatusUrl() {
    const url = new URL(RUNTIME_STATUS_PATH, window.location.origin);
    url.searchParams.set("_cb", String(Date.now()));
    return url.toString();
  }

  function getCurrentPageBootId() {
    const root = document.documentElement;
    if (!root || !root.dataset) {
      return "";
    }
    return String(root.dataset.bootId || "");
  }

  function probeRuntimeStatus() {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("GET", buildRuntimeStatusUrl(), true);
      xhr.timeout = RECONNECT_REQUEST_TIMEOUT_MS;
      xhr.onload = () => {
        let payload = null;
        try {
          payload = xhr.responseText ? JSON.parse(xhr.responseText) : null;
        } catch (_) {
          payload = null;
        }
        resolve({
          status: xhr.status,
          payload,
        });
      };
      xhr.onerror = () => reject(new Error("Runtime status probe failed"));
      xhr.ontimeout = () => reject(new Error("Runtime status probe timed out"));
      xhr.send();
    });
  }

  function startDisconnectWatch(options = {}) {
    clearDisconnectWatch();

    const delayMs = Number.isFinite(options.delayMs) ? Math.max(0, options.delayMs) : 0;
    const pollMs = Number.isFinite(options.pollMs)
      ? Math.max(100, options.pollMs)
      : DISCONNECT_WATCH_DEFAULT_POLL_MS;

    const tick = async () => {
      if (disconnectWatchInFlight || expectedDisconnect) {
        return;
      }

      disconnectWatchInFlight = true;
      try {
        const response = await probeRuntimeStatus();
        if (response.status >= 200 && response.status < 300) {
          recordConnectivitySuccess();
        } else {
          recordConnectivityFailure();
        }
      } catch (_) {
        recordConnectivityFailure();
      } finally {
        disconnectWatchInFlight = false;
      }
    };

    disconnectWatchDelayTimer = window.setTimeout(() => {
      disconnectWatchDelayTimer = null;
      tick();
      disconnectWatchPollTimer = window.setInterval(tick, pollMs);
    }, delayMs);
  }

  function makeAbortError(message) {
    if (typeof DOMException === "function") {
      return new DOMException(message || "Request aborted", "AbortError");
    }
    const error = new Error(message || "Request aborted");
    error.name = "AbortError";
    return error;
  }

  function ensureOverlay() {
    if (overlay && overlay.isConnected) {
      return overlay;
    }

    if (!document.getElementById(STYLE_ID)) {
      const popupStyle = document.createElement("style");
      popupStyle.id = STYLE_ID;
      popupStyle.textContent = `
        #${OVERLAY_ID} {
          position: fixed;
          top: 0;
          right: 0;
          bottom: 0;
          left: 0;
          width: 100vw;
          height: 100vh;
          display: none;
          align-items: center;
          justify-content: center;
          padding: 1rem;
          background: rgba(15, 23, 42, 0.45);
          z-index: 9999;
        }
        #${OVERLAY_ID}[data-visible="1"] {
          display: flex;
        }
        #coinbox-connection-lost-card {
          width: min(560px, 100%);
          background: #ffffff;
          border: 1px solid #fecaca;
          border-radius: 12px;
          box-shadow: 0 20px 45px rgba(15, 23, 42, 0.2);
          padding: 1.1rem 1.15rem;
        }
        #coinbox-connection-lost-card h2 {
          margin: 0 0 0.45rem;
          color: #991b1b;
          font-size: clamp(1.05rem, 1vw + 0.9rem, 1.35rem);
        }
        #coinbox-connection-lost-card p {
          margin: 0;
          color: #1f2937;
          line-height: 1.45;
          font-size: 0.98rem;
        }
      `;
      document.head.appendChild(popupStyle);
    }

    overlay = document.getElementById(OVERLAY_ID);
    if (overlay) {
      return overlay;
    }

    if (!document.body) {
      return null;
    }

    overlay = document.createElement("div");
    overlay.id = OVERLAY_ID;
    overlay.className = "glyph-overlay-exempt";
    overlay.dataset.visible = "0";
    overlay.setAttribute("aria-live", "assertive");
    overlay.innerHTML = `
      <div id="coinbox-connection-lost-card" role="alert" aria-atomic="true">
        <h2>Connection lost!</h2>
        <p>Make sure the coinbox is powered on and that your device is connected to the correct Wi-Fi network or Access Point. If you recently changed or reset the network settings, or were connected to the recovery Access Point, switch to the correct network instead.</p>
      </div>
    `;
    if (typeof document.body.prepend === "function") {
      document.body.prepend(overlay);
    } else if (document.body.firstChild) {
      document.body.insertBefore(overlay, document.body.firstChild);
    } else {
      document.body.appendChild(overlay);
    }
    return overlay;
  }

  function hideLostConnectionPopup() {
    const popup = ensureOverlay();
    if (popup) {
      popup.dataset.visible = "0";
    }
  }

  function showLostConnectionPopup() {
    const popup = ensureOverlay();
    if (popup) {
      popup.dataset.visible = "1";
    }
  }

  function markExpectedDisconnect() {
    expectedDisconnect = true;
    consecutiveFailures = FAILURE_THRESHOLD;
    expectedDisconnectBootId = getCurrentPageBootId();
    clearDisconnectWatch();
    showLostConnectionPopup();
    if (reconnectPollTimer !== null) {
      return;
    }
    reconnectPollTimer = window.setInterval(async () => {
      if (!expectedDisconnect || reconnectPollInFlight) {
        return;
      }

      reconnectPollInFlight = true;
      try {
        const response = await probeRuntimeStatus();
        if (!expectedDisconnect || reconnectNavigationPending) {
          return;
        }
        if (response.status < 200 || response.status >= 300) {
          return;
        }
        const nextBootId = response && response.payload && typeof response.payload.boot_id === "string"
          ? response.payload.boot_id
          : "";
        const nextReady = !!(response && response.payload && response.payload.ready === true);
        if (!nextBootId || !nextReady) {
          return;
        }
        if (expectedDisconnectBootId && nextBootId === expectedDisconnectBootId) {
          return;
        }
        reconnectNavigationPending = true;
        window.location.replace("/");
      } catch (_) {
        // keep polling until the device becomes reachable again
      } finally {
        reconnectPollInFlight = false;
      }
    }, RECONNECT_POLL_MS);
  }

  function clearExpectedDisconnect() {
    expectedDisconnect = false;
    consecutiveFailures = 0;
    clearReconnectPolling();
    clearDisconnectWatch();
    hideLostConnectionPopup();
  }

  function recordConnectivitySuccess() {
    if (expectedDisconnect) {
      return;
    }
    consecutiveFailures = 0;
    hideLostConnectionPopup();
  }

  function recordConnectivityFailure() {
    consecutiveFailures += 1;
    if (consecutiveFailures >= FAILURE_THRESHOLD) {
      showLostConnectionPopup();
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", ensureOverlay, { once: true });
  } else {
    ensureOverlay();
  }

  window.COINBOX_CONNECTION_MONITOR = Object.freeze({
    showLostConnectionPopup,
    hideLostConnectionPopup,
    expectDisconnect: markExpectedDisconnect,
    clearExpectedDisconnect,
    startDisconnectWatch,
    clearDisconnectWatch,
  });

  window.fetch = (...args) => {
    const init = args.length > 1 && args[1] && typeof args[1] === "object" ? args[1] : null;
    if (expectedDisconnect && !(init && init[FETCH_ALLOW_DURING_EXPECTED_DISCONNECT] === true)) {
      return Promise.reject(makeAbortError("Planned reconnect in progress"));
    }
    return nativeFetch(...args)
      .then((response) => {
        recordConnectivitySuccess();
        return response;
      })
      .catch((error) => {
        if (!error || error.name !== "AbortError") {
          recordConnectivityFailure();
        }
        throw error;
      });
  };
})();
