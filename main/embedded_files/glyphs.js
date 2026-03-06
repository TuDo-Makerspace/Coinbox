(function initCoinboxGlyphs() {
  const BASE_CONFIG = Object.freeze({
    density: 0.18,
    minCols: 8,
    minRows: 6,
    colWidthPx: 52,
    rowHeightPx: 40,
    minCount: 8,
    opacity: 0.24,
    glyphs: Object.freeze([
      Object.freeze({ text: '[?]', sizePx: 18 }),
      Object.freeze({ text: '$', sizePx: 18 }),
      Object.freeze({ text: '€', sizePx: 18 }),
      Object.freeze({ text: '♫', sizePx: 26 }),
      Object.freeze({ text: '♪', sizePx: 26 }),
    ]),
  });

  function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
  }

  function sprinkleBackgroundGlyphs(overrideConfig) {
    if (!document.body || document.querySelector('.bg-glyph-layer')) return;

    const cfg = Object.assign({}, BASE_CONFIG, overrideConfig || {});
    const densityRaw = Number(cfg.density);
    const density = Number.isFinite(densityRaw) ? clamp(densityRaw, 0, 1) : BASE_CONFIG.density;
    const cols = Math.max(cfg.minCols, Math.floor(window.innerWidth / cfg.colWidthPx));
    const rows = Math.max(cfg.minRows, Math.floor(window.innerHeight / cfg.rowHeightPx));
    const cellCount = cols * rows;
    const count = Math.min(cellCount, Math.max(cfg.minCount, Math.floor(cellCount * density)));
    const slots = Array.from({ length: cellCount }, (_, i) => i);
    const glyphChoices = Array.isArray(cfg.glyphs) && cfg.glyphs.length ? cfg.glyphs : BASE_CONFIG.glyphs;

    for (let i = slots.length - 1; i > 0; i -= 1) {
      const j = Math.floor(Math.random() * (i + 1));
      const tmp = slots[i];
      slots[i] = slots[j];
      slots[j] = tmp;
    }

    const layer = document.createElement('div');
    layer.className = 'bg-glyph-layer';
    layer.setAttribute('aria-hidden', 'true');

    for (let i = 0; i < count; i += 1) {
      const slot = slots[i];
      const col = slot % cols;
      const row = Math.floor(slot / cols);
      const choice = glyphChoices[Math.floor(Math.random() * glyphChoices.length)] || BASE_CONFIG.glyphs[0];
      const sizePx = Number(choice.sizePx);
      const glyph = document.createElement('span');
      glyph.className = 'bg-glyph';
      glyph.textContent = choice.text || '[?]';
      glyph.style.left = `${((col + 0.5) / cols) * 100}%`;
      glyph.style.top = `${((row + 0.5) / rows) * 100}%`;
      glyph.style.opacity = String(cfg.opacity);
      glyph.style.fontSize = `${Number.isFinite(sizePx) ? sizePx : 18}px`;
      glyph.style.transform = 'translate(-50%, -50%)';
      layer.appendChild(glyph);
    }

    document.body.prepend(layer);
  }

  window.COINBOX_GLYPHS = Object.freeze({
    config: BASE_CONFIG,
    sprinkleBackgroundGlyphs,
  });
})();
