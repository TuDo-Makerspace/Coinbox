(function () {
  if (window.__coinboxConnectionMonitorInstalled || typeof window.fetch !== "function") {
    return;
  }
  window.__coinboxConnectionMonitorInstalled = true;

  const OVERLAY_ID = "coinbox-connection-lost-overlay";
  const STYLE_ID = "coinbox-connection-style";
  const FAILURE_THRESHOLD = 2;
  let consecutiveFailures = 0;
  let overlay = null;

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
        <p>Please make sure the coinbox is powered on and that you are connected to the correct AP or STA network, especially if you recently changed or reset the network settings or were previously connected to the recovery AP.</p>
      </div>
    `;
    document.body.appendChild(overlay);
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

  function recordConnectivitySuccess() {
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

  const nativeFetch = window.fetch.bind(window);
  window.fetch = (...args) => {
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
