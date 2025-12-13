(function () {
  const container = document.getElementById("navbar");
  if (!container) return;

  if (!document.getElementById("navbar-shared-style")) {
    const style = document.createElement("style");
    style.id = "navbar-shared-style";
    style.textContent = `
      nav {
        background: #333;
        padding: 0;
        display: flex;
        gap: 0.6em;
        justify-content: center;
        align-items: stretch;
        flex-wrap: nowrap;
        font-family: sans-serif;
        box-shadow: 0 2px 8px rgba(0, 0, 0, 0.12);
        width: 100%;
        margin: 0;
        position: relative;
        overflow: hidden;
      }
      nav a {
        color: #fff;
        text-decoration: none;
        padding: clamp(0.65em, 1.5vw, 0.95em) clamp(0.8em, 2vw, 1.05em);
        border-radius: 0;
        font-weight: bold;
        background: transparent;
        transition: background 0.2s, transform 0.1s, color 0.2s;
        text-align: center;
        font-size: clamp(0.88rem, 2.4vw + 0.3rem, 1.12rem);
        display: flex;
        align-items: center;
        justify-content: center;
        width: 100%;
        height: 100%;
        min-height: 52px;
        flex: 1 1 0;
        min-width: 0;
        position: relative;
      }
      nav a::after {
        content: "";
        position: absolute;
        left: 0;
        right: 0;
        bottom: -1px;
        height: 5px;
        background: transparent;
        transition: background 0.2s;
      }
      nav a:hover,
      nav a.active {
        transform: none;
        background: transparent;
      }
      nav a:hover {
        color: #f9fafb;
        background: transparent;
      }
      nav a.active {
        color: #f9fafb;
        background: transparent;
      }
      nav a:hover::after {
        background: #60a5fa;
      }
      nav a.active::after {
        background: #60a5fa;
      }
      .hovering-other a.active::after {
        background: transparent;
      }
      nav a:active {
        transform: scale(0.99);
      }
      .hovering-other a.active {
        /* keep underline while hovering elsewhere */
      }
      @media (max-width: 640px) {
        nav {
          gap: 0.35em;
          padding: 0;
        }
        nav a {
          padding: 0.7em 0.75em;
          font-size: 0.92rem;
          min-height: 46px;
        }
        nav a::after {
          height: 4px;
          bottom: -1px;
        }
      }
    `;
    document.head.appendChild(style);
  }

  const links = [
    { href: "/", label: "Coinbox", isActive: (path) => path === "/" },
    { href: "/file-server/", label: "File Server", isActive: (path) => path.startsWith("/file-server") },
    { href: "/mt/", label: "Maintenance", isActive: (path) => path.startsWith("/mt") },
  ];

  const nav = document.createElement("nav");
  const path = window.location.pathname || "/";

  links.forEach((link) => {
    const a = document.createElement("a");
    a.href = link.href;
    a.textContent = link.label;
    if (link.isActive(path)) {
      a.classList.add("active");
    }
    nav.appendChild(a);
  });

  container.appendChild(nav);

  nav.addEventListener("mouseover", (e) => {
    const target = e.target;
    if (target && target.tagName === "A" && !target.classList.contains("active")) {
      nav.classList.add("hovering-other");
    }
  });
  nav.addEventListener("mouseout", (e) => {
    const related = e.relatedTarget;
    const leavingNav = !related || !nav.contains(related);
    if (leavingNav || (related && related.tagName === "A" && related.classList.contains("active"))) {
      nav.classList.remove("hovering-other");
    }
  });
})();
