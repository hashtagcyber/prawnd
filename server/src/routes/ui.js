const UI_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Prawnd</title>
<style>
  :root { color-scheme: light dark; }
  * { box-sizing: border-box; }
  body { font-family: system-ui, -apple-system, sans-serif; max-width: 960px; margin: 1rem auto; padding: 0 1rem; }
  h1 { margin: 0 0 .25rem; font-size: 1.5rem; }
  .sub { opacity: .6; font-size: .875rem; margin-bottom: 1rem; }
  .toolbar { display: flex; gap: .5rem; align-items: center; flex-wrap: wrap; margin-bottom: 1rem; }
  .toolbar input, .toolbar button { padding: .5rem .6rem; font: inherit; border-radius: 6px; border: 1px solid #8884; background: transparent; color: inherit; }
  .toolbar input { flex: 1 1 200px; min-width: 0; }
  .toolbar button { cursor: pointer; }
  .status { font-size: .875rem; opacity: .7; flex: 1 0 100%; }
  .status.err { color: crimson; opacity: 1; }
  .rec { border: 1px solid #8884; border-radius: 8px; padding: .75rem 1rem; margin-bottom: .75rem; }
  .rec header { display: flex; justify-content: space-between; gap: 1rem; flex-wrap: wrap; align-items: baseline; margin-bottom: .5rem; }
  .rec .id { font-family: ui-monospace, SFMono-Regular, monospace; font-size: .8rem; opacity: .6; }
  .rec .meta { font-size: .875rem; opacity: .8; display: flex; gap: .75rem; flex-wrap: wrap; }
  .rec audio { width: 100%; margin-top: .25rem; }
  .rec .row { display: flex; gap: 1rem; align-items: center; flex-wrap: wrap; }
  .rec a { font-size: .875rem; }
  .rec .variants { display: flex; gap: 1rem; margin-top: .5rem; flex-wrap: wrap; }
  .rec .variants > div { flex: 1 1 280px; }
  .rec .variants label { font-size: .75rem; opacity: .7; text-transform: uppercase; letter-spacing: .04em; }
  .empty { text-align: center; padding: 2rem; opacity: .6; }
  #more { width: 100%; padding: .6rem; font: inherit; border-radius: 6px; border: 1px solid #8884; background: transparent; color: inherit; cursor: pointer; }
</style>
</head>
<body>
<h1>Prawnd</h1>
<div class="sub">Recordings indexed by the server. Enter your API key to load.</div>
<div class="toolbar">
  <input id="key" type="password" placeholder="API key" autocomplete="off">
  <input id="device" type="text" placeholder="Filter by device id (optional)">
  <button id="refresh">Refresh</button>
  <div class="status" id="status"></div>
</div>
<div id="list"></div>
<button id="more" style="display:none">Load more</button>

<script>
const $ = (s) => document.querySelector(s);
const keyEl = $('#key');
const deviceEl = $('#device');
const listEl = $('#list');
const statusEl = $('#status');
const moreBtn = $('#more');

keyEl.value = localStorage.getItem('prawnd_api_key') || '';
deviceEl.value = localStorage.getItem('prawnd_device') || '';
keyEl.addEventListener('change', () => { localStorage.setItem('prawnd_api_key', keyEl.value); load(true); });
deviceEl.addEventListener('change', () => { localStorage.setItem('prawnd_device', deviceEl.value); load(true); });
$('#refresh').addEventListener('click', () => load(true));
moreBtn.addEventListener('click', () => load(false));

let nextCursor = null;

const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
}[c]));

const fmtBytes = (n) =>
  n < 1024 ? n + ' B'
  : n < 1024 * 1024 ? (n / 1024).toFixed(1) + ' KB'
  : (n / 1048576).toFixed(1) + ' MB';

const fmtDuration = (s) => {
  if (!s) return '0:00';
  const m = Math.floor(s / 60);
  const r = Math.round(s - m * 60);
  return m + ':' + String(r).padStart(2, '0');
};

const fmtTime = (ms) => new Date(ms).toLocaleString();

function setStatus(text, isError) {
  statusEl.textContent = text;
  statusEl.className = 'status' + (isError ? ' err' : '');
}

function render(items, append) {
  if (!append) listEl.innerHTML = '';
  if (!items.length && !append) {
    listEl.innerHTML = '<div class="empty">No recordings yet.</div>';
    return;
  }
  const keyQs = encodeURIComponent(keyEl.value);
  for (const r of items) {
    const origUrl = '/recordings/' + encodeURIComponent(r.id) + '/file?api_key=' + keyQs;
    const cleanUrl = origUrl + '&variant=cleaned';
    const hasCleaned = !!r.cleaned_path;
    const div = document.createElement('div');
    div.className = 'rec';
    let variantsHtml;
    if (hasCleaned) {
      variantsHtml =
        '<div class="variants">' +
          '<div><label>Cleaned</label>' +
            '<audio controls preload="none" src="' + cleanUrl + '"></audio>' +
            '<a href="' + cleanUrl + '" download>download cleaned</a></div>' +
          '<div><label>Original</label>' +
            '<audio controls preload="none" src="' + origUrl + '"></audio>' +
            '<a href="' + origUrl + '" download>download original</a></div>' +
        '</div>';
    } else {
      variantsHtml =
        '<audio controls preload="none" src="' + origUrl + '"></audio>' +
        '<div style="font-size:.75rem;opacity:.6;margin-top:.25rem">Cleaned version not available — playing original.</div>';
    }
    div.innerHTML =
      '<header>' +
        '<div class="row">' +
          '<strong>' + escapeHtml(r.device_id) + '</strong>' +
          '<span class="id">' + escapeHtml(r.id) + '</span>' +
        '</div>' +
        '<div class="meta">' +
          '<span>' + escapeHtml(fmtTime(r.received_at)) + '</span>' +
          '<span>' + escapeHtml(fmtDuration(r.duration_sec)) + '</span>' +
          '<span>' + escapeHtml(fmtBytes(r.size_bytes)) + '</span>' +
          '<span>' + escapeHtml(r.sample_rate + ' Hz × ' + r.channels + 'ch') + '</span>' +
        '</div>' +
      '</header>' +
      variantsHtml;
    listEl.appendChild(div);
  }
}

async function load(reset) {
  if (!keyEl.value) {
    setStatus('Enter an API key to load recordings.', true);
    return;
  }
  setStatus('Loading…');
  try {
    const params = new URLSearchParams();
    if (deviceEl.value) params.set('device_id', deviceEl.value);
    if (!reset && nextCursor) params.set('cursor', nextCursor);
    params.set('limit', '50');
    const resp = await fetch('/recordings?' + params.toString(), {
      headers: { Authorization: 'Bearer ' + keyEl.value },
    });
    if (resp.status === 401) {
      setStatus('Unauthorized — check the API key.', true);
      return;
    }
    if (!resp.ok) {
      setStatus('Server returned ' + resp.status, true);
      return;
    }
    const data = await resp.json();
    render(data.items, !reset && nextCursor);
    nextCursor = data.next_cursor;
    moreBtn.style.display = nextCursor ? 'block' : 'none';
    setStatus(data.items.length + ' recording(s) loaded');
  } catch (e) {
    setStatus('Error: ' + e.message, true);
  }
}

if (keyEl.value) load(true);
else setStatus('Enter an API key to load recordings.', true);
</script>
</body>
</html>
`;

export default async function uiRoute(app) {
  app.get('/', async (req, reply) => {
    reply.type('text/html; charset=utf-8').send(UI_HTML);
  });
}
