const express = require('express');
const path = require('path');
const fs = require('fs');

const app = express();
const PORT = process.env.PORT || 3000;

app.use(express.json());

const embeddedDir = path.join(__dirname, 'main', 'embedded_files');
const fileserverPath = path.join(embeddedDir, 'fileserver.html');
const navbarPath = path.join(embeddedDir, 'navbar.js');

const metaStore = new Map();
const entries = [
  { name: 'alpha.bin', probability: 40, volume: 60, enabled: true },
  { name: 'beta.mp3', probability: 75, volume: 80, enabled: false },
  { name: 'gamma.txt', probability: 20, volume: 50, enabled: true },
];

entries.forEach((e) => {
  metaStore.set(e.name, { probability: e.probability, volume: e.volume, enabled: e.enabled });
});

function renderEntry(entry, idx) {
  const rowId = `row-${idx}`;
  const p = entry.probability ?? 50;
  const v = entry.volume ?? 50;
  const en = entry.enabled ? 1 : 0;
  return `
    <div class="file-item" id="${rowId}"
         data-file-uri="/file-server/${entry.name}"
         data-probability="${p}"
         data-volume="${v}"
         data-enabled="${en}">
      <div class="file-row-main">
        <a class="file-name" href="/file-server/${entry.name}" title="${entry.name}">${entry.name}</a>
        <div class="row-actions">
          <label class="switch"><input type="checkbox" data-k="enabled-toggle"><span class="slider"></span></label>
          <button class="chev" data-row-id="${rowId}" data-open="0">&#9881;</button>
        </div>
      </div>
      <div class="details" data-for-row="${rowId}">
        <div class="details-inner">
          <div class="prop prob-row"><label>Probability</label>
            <div class="slider-wrap">
              <button class="nudge" data-k="probability-minus">-</button>
              <input type="range" min="0" max="100" data-k="probability">
              <button class="nudge" data-k="probability-plus">+</button>
            </div>
            <input type="number" min="0" max="100" data-k="probability-num">
            <span class="percent">%</span>
          </div>

          <div class="prop vol-row"><label>Volume</label>
            <div class="slider-wrap">
              <button class="nudge" data-k="volume-minus">-</button>
              <input type="range" min="0" max="100" data-k="volume">
              <button class="nudge" data-k="volume-plus">+</button>
            </div>
            <input type="number" min="0" max="100" data-k="volume-num">
            <span class="percent">%</span>
          </div>
        </div>
      </div>
    </div>`;
}

app.get('/navbar.js', (_req, res) => res.sendFile(navbarPath));

app.get('/file-server/', (_req, res) => {
  const base = fs.readFileSync(fileserverPath, 'utf8');
  let out = base;
  out += `<script>window.CURRENT_PATH='/file-server/';</script>`;
  entries.forEach((e, idx) => {
    const stored = metaStore.get(e.name) || e;
    out += renderEntry(stored, idx);
  });
  out += '</div></div></body></html>';
  res.type('html').send(out);
});

// mock upload: add entry
app.post('/file-server/:name', (req, res) => {
  const name = req.params.name;
  if (!name) return res.status(400).send('no name');
  if (!metaStore.has(name)) {
    metaStore.set(name, { probability: 50, volume: 50, enabled: true, name });
    entries.push({ name, probability: 50, volume: 50, enabled: true });
  }
  res.send('ok');
});

app.get('/file-meta/*', (req, res) => {
  const name = path.basename(req.path);
  const meta = metaStore.get(name);
  if (!meta) return res.status(404).json({ error: 'not found' });
  res.json({
    probability: meta.probability ?? 50,
    volume: meta.volume ?? 50,
    enabled: !!meta.enabled,
  });
});

app.post('/file-meta/*', (req, res) => {
  const name = path.basename(req.path);
  const meta = metaStore.get(name);
  if (!meta) return res.status(404).json({ error: 'not found' });
  meta.probability = Math.min(100, Math.max(0, parseInt(req.body.probability ?? meta.probability, 10)));
  meta.volume = Math.min(100, Math.max(0, parseInt(req.body.volume ?? meta.volume, 10)));
  meta.enabled = !!req.body.enabled;
  metaStore.set(name, meta);
  res.send('OK');
});

app.post('/format', (_req, res) => {
  entries.length = 0;
  metaStore.clear();
  res.send('formatted (mock)');
});

app.get('/', (_req, res) => res.redirect('/file-server/'));

app.listen(PORT, () => {
  console.log(`Mock file server UI running at http://localhost:${PORT}/file-server/`);
});
