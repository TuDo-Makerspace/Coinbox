(function () {
  const container = document.getElementById("navbar");
  if (!container) return;

  if (!document.getElementById("navbar-shared-style")) {
    const style = document.createElement("style");
    style.id = "navbar-shared-style";
    style.textContent = `
      nav {
        background: #333;
        padding: 1em;
        display: flex;
        justify-content: center;
        font-family: sans-serif;
      }
      nav a {
        color: #fff;
        text-decoration: none;
        margin: 0 1em;
        padding: 0.5em 1em;
        border-radius: 4px;
        font-weight: bold;
        transition: background 0.2s;
      }
      nav a:hover,
      nav a.active {
        background: #555;
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
})();
