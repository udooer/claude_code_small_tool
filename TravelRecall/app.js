(function () {
  'use strict';

  const $ = id => document.getElementById(id);
  const HOUR = 3600 * 1000;
  const WEEK = ['日', '一', '二', '三', '四', '五', '六'];

  let items = [];          // 所有載入的檔案
  let pendingTrip = null;  // 先匯入行程檔、還沒選照片時暫存
  let videoOffset = 0;     // 影片（UTC 時間）校正小時數

  // ================================================================
  // 共用
  // ================================================================
  function effTime(it) {
    return it.time + (it.timeSource === 'video-utc' ? videoOffset * HOUR : 0);
  }
  function pad(n) { return String(n).padStart(2, '0'); }
  function dayKey(t) { const d = new Date(t); return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`; }
  function fmtDay(t) { const d = new Date(t); return `${d.getFullYear()}/${pad(d.getMonth() + 1)}/${pad(d.getDate())}（${WEEK[d.getDay()]}）`; }
  function fmtTime(t) { const d = new Date(t); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; }
  function sorted(list) { return list.slice().sort((a, b) => effTime(a) - effTime(b) || a.name.localeCompare(b.name)); }
  function hasLoc(it) { return it.lat != null && it.lng != null; }
  function escapeHtml(s) { return s.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]); }

  // 縮圖延遲載入：進到畫面才讀
  const thumbObserver = new IntersectionObserver(entries => {
    for (const e of entries) {
      if (!e.isIntersecting) continue;
      thumbObserver.unobserve(e.target);
      fillThumb(e.target, byId(e.target.dataset.id));
    }
  }, { rootMargin: '400px' });

  const idMap = new Map();
  function byId(id) { return idMap.get(id); }

  async function fillThumb(box, it) {
    if (!it) return;
    if (it.kind === 'video') {
      const v = document.createElement('video');
      v.muted = true; v.preload = 'metadata'; v.playsInline = true;
      v.src = Media.objectUrl(it) + '#t=0.5';
      v.onerror = () => { box.innerHTML = '<div class="ph">🎬</div>'; };
      box.replaceChildren(v);
      return;
    }
    const url = await Media.thumbUrl(it);
    if (!it.renderable && !it.thumbIsExif) { box.innerHTML = `<div class="ph">${escapeHtml(it.name.split('.').pop().toUpperCase())}</div>`; return; }
    const img = new Image();
    img.decoding = 'async';
    img.src = url;
    const rot = Media.thumbRotation(it);
    if (rot) img.style.transform = `rotate(${rot}deg)`;
    img.onerror = () => { box.innerHTML = '<div class="ph">🖼️</div>'; };
    box.replaceChildren(img);
  }

  // ================================================================
  // 第一段：挑選
  // ================================================================
  async function addFiles(fileList) {
    const files = Array.from(fileList);
    if (!files.length) return;
    const prog = $('progress');
    prog.hidden = false;
    const bar = prog.querySelector('.bar'), label = prog.querySelector('.label');
    const loaded = await Media.loadFiles(files, (d, n) => {
      bar.style.width = (d / n * 100) + '%';
      label.textContent = `讀取中 ${d} / ${n}`;
    });
    prog.hidden = true;

    const known = new Set(items.map(it => it.key));
    let added = 0;
    for (const it of loaded) {
      if (known.has(it.key)) continue;
      items.push(it); idMap.set(it.id, it); known.add(it.key); added++;
    }
    if (!added && files.length) alert('沒有找到新的照片或影片。');
    if (pendingTrip) { applyTrip(pendingTrip); pendingTrip = null; }
    renderGrid();
  }

  function renderGrid() {
    const grid = $('grid');
    const onlyGps = $('onlyGpsChk').checked;
    $('emptyHint').hidden = items.length > 0;
    $('pickToolbar').hidden = items.length === 0;
    $('exportBtn').disabled = items.length === 0;

    const groups = new Map();
    for (const it of sorted(items)) {
      if (onlyGps && !hasLoc(it)) continue;
      const k = dayKey(effTime(it));
      if (!groups.has(k)) groups.set(k, []);
      groups.get(k).push(it);
    }

    const frag = document.createDocumentFragment();
    let dayNo = 0;
    for (const [, list] of groups) {
      dayNo++;
      const sec = document.createElement('section');
      sec.className = 'day';
      const head = document.createElement('div');
      head.className = 'day-head';
      head.innerHTML = `<b>第 ${dayNo} 天</b> <span>${fmtDay(effTime(list[0]))}</span>
        <span class="muted day-count"></span>
        <button class="btn small ghost day-toggle">整天全選 / 取消</button>`;
      head.querySelector('.day-toggle').onclick = () => {
        const target = !list.every(it => it.selected);
        list.forEach(it => { it.selected = target; syncCard(it); });
        updateStats();
      };
      sec.appendChild(head);
      const row = document.createElement('div');
      row.className = 'cards';
      for (const it of list) row.appendChild(makeCard(it));
      sec.appendChild(row);
      sec._list = list;
      frag.appendChild(sec);
    }
    grid.replaceChildren(frag);
    grid.querySelectorAll('.thumb').forEach(el => thumbObserver.observe(el));
    updateStats();
  }

  function makeCard(it) {
    const c = document.createElement('div');
    c.className = 'item';
    c.dataset.id = it.id;
    c.innerHTML = `
      <div class="thumb" data-id="${escapeHtml(it.id)}"></div>
      ${it.kind === 'video' ? '<span class="tag video">▶ 影片</span>' : ''}
      <span class="check">✓</span>
      <div class="info">
        <span class="t">${fmtTime(effTime(it))}</span>
        <span class="loc"></span>
      </div>
      <div class="cap"></div>
      <div class="tools">
        <button class="mini" data-act="cap" title="寫一句回憶">✎</button>
        <button class="mini" data-act="loc" title="設定位置">📍</button>
      </div>`;
    c.onclick = e => {
      const act = e.target.dataset && e.target.dataset.act;
      if (act === 'cap') {
        const v = prompt(`為「${it.name}」寫一句回憶：`, it.caption);
        if (v !== null) { it.caption = v.trim(); syncCard(it); }
      } else if (act === 'loc') {
        openLocModal(it);
      } else {
        it.selected = !it.selected;
        syncCard(it);
        updateStats();
      }
    };
    c._item = it;
    syncCard(it, c);
    return c;
  }

  function cardOf(it) { return $('grid').querySelector(`.item[data-id="${CSS.escape(it.id)}"]`); }

  function syncCard(it, el) {
    el = el || cardOf(it);
    if (!el) return;
    el.classList.toggle('selected', it.selected);
    const loc = el.querySelector('.loc');
    loc.textContent = hasLoc(it) ? (it.locSource === 'manual' ? '📍手動' : '📍') : '無定位';
    loc.classList.toggle('missing', !hasLoc(it));
    el.querySelector('.cap').textContent = it.caption;
    el.title = `${it.name}\n${fmtDay(effTime(it))} ${fmtTime(effTime(it))}` +
      (it.timeSource === 'file' ? '\n（無拍攝時間，使用檔案修改時間）' : '');
  }

  function updateStats() {
    const sel = items.filter(it => it.selected);
    const noLoc = sel.filter(it => !hasLoc(it)).length;
    $('pickStats').textContent = `已選 ${sel.length} / ${items.length}` + (noLoc ? `（${noLoc} 張無定位，會依時間推算位置）` : '');
    $('startBtn').disabled = !sel.some(hasLoc);
    $('startBtn').title = sel.some(hasLoc) ? '' : '至少要有一張已選的照片帶有位置';
    document.querySelectorAll('#grid .day').forEach(sec => {
      const l = sec._list;
      sec.querySelector('.day-count').textContent = `已選 ${l.filter(i => i.selected).length} / ${l.length}`;
    });
  }

  // ---------- 檔案輸入 / 拖放 ----------
  $('dirInput').onchange = e => { addFiles(e.target.files); e.target.value = ''; };
  $('fileInput').onchange = e => { addFiles(e.target.files); e.target.value = ''; };

  const dz = $('dropzone');
  dz.addEventListener('dragover', e => { e.preventDefault(); dz.classList.add('over'); });
  dz.addEventListener('dragleave', () => dz.classList.remove('over'));
  dz.addEventListener('drop', async e => {
    e.preventDefault();
    dz.classList.remove('over');
    const entries = Array.from(e.dataTransfer.items || []).map(i => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
    if (!entries.length) { addFiles(e.dataTransfer.files); return; }
    const files = [];
    await Promise.all(entries.map(en => walkEntry(en, files)));
    addFiles(files);
  });

  function walkEntry(entry, out) {
    return new Promise(resolve => {
      if (entry.isFile) {
        entry.file(f => { out.push(f); resolve(); }, () => resolve());
      } else if (entry.isDirectory) {
        const reader = entry.createReader();
        const all = [];
        const readBatch = () => reader.readEntries(async batch => {
          if (!batch.length) { await Promise.all(all.map(en => walkEntry(en, out))); resolve(); return; }
          all.push(...batch);
          readBatch();
        }, () => resolve());
        readBatch();
      } else resolve();
    });
  }

  // ---------- 工具列 ----------
  $('selAllBtn').onclick = () => { items.forEach(it => it.selected = true); items.forEach(it => syncCard(it)); updateStats(); };
  $('selNoneBtn').onclick = () => { items.forEach(it => it.selected = false); items.forEach(it => syncCard(it)); updateStats(); };
  $('onlyGpsChk').onchange = renderGrid;
  $('videoOffset').onchange = e => { videoOffset = parseFloat(e.target.value) || 0; renderGrid(); };

  // ---------- 行程檔 匯出 / 匯入 ----------
  $('exportBtn').onclick = () => {
    const data = {
      app: 'TravelRecall', version: 1, videoOffset,
      items: items.map(it => ({
        key: it.key, selected: it.selected,
        caption: it.caption || undefined,
        lat: it.locSource === 'manual' ? it.lat : undefined,
        lng: it.locSource === 'manual' ? it.lng : undefined,
      })),
    };
    const blob = new Blob([JSON.stringify(data, null, 1)], { type: 'application/json' });
    const a = document.createElement('a');
    const days = sorted(items);
    a.download = `trip_${days.length ? dayKey(effTime(days[0])) : 'export'}.json`;
    a.href = URL.createObjectURL(blob);
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  };

  $('tripInput').onchange = async e => {
    const f = e.target.files[0];
    e.target.value = '';
    if (!f) return;
    let data;
    try { data = JSON.parse(await f.text()); } catch (err) { alert('行程檔格式不正確'); return; }
    if (!data || data.app !== 'TravelRecall') { alert('這不是旅途回憶的行程檔'); return; }
    if (!items.length) { pendingTrip = data; alert('已讀取行程檔，接著請選擇同一批照片的資料夾。'); return; }
    applyTrip(data);
    renderGrid();
  };

  function applyTrip(data) {
    videoOffset = data.videoOffset || 0;
    $('videoOffset').value = videoOffset;
    const map = new Map((data.items || []).map(r => [r.key, r]));
    let matched = 0;
    for (const it of items) {
      const r = map.get(it.key);
      if (!r) continue;
      matched++;
      it.selected = !!r.selected;
      it.caption = r.caption || '';
      if (Media.validLatLng(r.lat, r.lng)) { it.lat = r.lat; it.lng = r.lng; it.locSource = 'manual'; }
    }
    if (!matched) alert('行程檔裡的檔案和目前的照片對不起來（檔名或大小不同）。');
  }

  // ================================================================
  // 手動設定位置
  // ================================================================
  let locMap, locMarker, locItem, locPick;

  function openLocModal(it) {
    locItem = it;
    locPick = hasLoc(it) ? L.latLng(it.lat, it.lng) : null;
    $('locName').textContent = `${it.name}　${fmtDay(effTime(it))} ${fmtTime(effTime(it))}`;
    $('locModal').hidden = false;
    if (!locMap) {
      locMap = L.map('locMap', { worldCopyJump: true });
      tileLayer('voyager').addTo(locMap);
      locMap.on('click', e => setLocPick(e.latlng));
    }
    setTimeout(() => {
      locMap.invalidateSize();
      const ref = locPick || nearestKnown(it);
      if (ref) locMap.setView(ref, locPick ? 15 : 12);
      else locMap.setView([23.7, 121], 3);
      setLocPick(locPick);
    }, 0);
    $('locSearch').value = '';
    $('locSearch').focus();
  }

  function setLocPick(ll) {
    locPick = ll;
    if (locMarker) { locMarker.remove(); locMarker = null; }
    if (ll) locMarker = L.marker(ll).addTo(locMap);
  }

  function nearestKnown(it) {
    let best = null, bestDt = Infinity;
    for (const o of items) {
      if (o === it || !hasLoc(o)) continue;
      const dt = Math.abs(effTime(o) - effTime(it));
      if (dt < bestDt) { bestDt = dt; best = o; }
    }
    return best ? L.latLng(best.lat, best.lng) : null;
  }

  function closeLoc() { $('locModal').hidden = true; locItem = null; }
  $('locClose').onclick = closeLoc;
  $('locModal').onclick = e => { if (e.target === $('locModal')) closeLoc(); };
  $('locOk').onclick = () => {
    if (locItem) {
      if (locPick) { locItem.lat = locPick.lat; locItem.lng = locPick.lng; locItem.locSource = 'manual'; }
      syncCard(locItem); updateStats();
    }
    closeLoc();
  };
  $('locClear').onclick = () => {
    if (locItem) { locItem.lat = locItem.lng = null; locItem.locSource = null; syncCard(locItem); updateStats(); }
    closeLoc();
  };
  $('locPrevBtn').onclick = () => {
    const ll = locItem && nearestKnown(locItem);
    if (ll) { setLocPick(ll); locMap.setView(ll, 15); } else alert('其他照片都沒有位置');
  };
  const doSearch = async () => {
    const q = $('locSearch').value.trim();
    if (!q) return;
    try {
      const r = await fetch(`https://nominatim.openstreetmap.org/search?format=json&limit=1&q=${encodeURIComponent(q)}`,
        { headers: { 'Accept-Language': 'zh-TW,zh,en' } });
      const j = await r.json();
      if (!j.length) { alert('找不到這個地點'); return; }
      const ll = L.latLng(+j[0].lat, +j[0].lon);
      setLocPick(ll);
      locMap.setView(ll, 15);
    } catch (e) { alert('搜尋失敗，請直接在地圖上點選'); }
  };
  $('locSearchBtn').onclick = doSearch;
  $('locSearch').onkeydown = e => { if (e.key === 'Enter') doSearch(); };

  // ================================================================
  // 第二段：地圖回憶
  // ================================================================
  const TILES = {
    voyager: ['https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png', '&copy; OpenStreetMap &copy; CARTO', 20],
    osm: ['https://tile.openstreetmap.org/{z}/{x}/{y}.png', '&copy; OpenStreetMap contributors', 19],
    sat: ['https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', 'Tiles &copy; Esri', 19],
    dark: ['https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', '&copy; OpenStreetMap &copy; CARTO', 20],
  };
  function tileLayer(name) {
    const [url, attribution, maxZoom] = TILES[name];
    return L.tileLayer(url, { attribution, maxZoom, subdomains: 'abcd' });
  }

  let map, baseLayer, routeAll, routeDone, curMarker, dotLayer;
  let play = [];        // 播放清單：{ it, ll, interp, day, dayNo }
  let cur = -1;
  let playing = false;
  let timer = null;
  let animToken = 0;

  function ensureMap() {
    if (map) return;
    map = L.map('map', { zoomControl: false, worldCopyJump: true });
    L.control.zoom({ position: 'topright' }).addTo(map);
    baseLayer = tileLayer('voyager').addTo(map);
    routeAll = L.polyline([], { color: '#8a8f98', weight: 3, opacity: 0.6, dashArray: '4 8' }).addTo(map);
    routeDone = L.polyline([], { color: '#ff6a3d', weight: 5, opacity: 0.9 }).addTo(map);
    dotLayer = L.layerGroup().addTo(map);
    $('tileSelect').onchange = e => { baseLayer.remove(); baseLayer = tileLayer(e.target.value).addTo(map); baseLayer.bringToBack(); };
  }

  function buildPlaylist() {
    const list = sorted(items.filter(it => it.selected));
    const known = list.map((it, i) => hasLoc(it) ? i : -1).filter(i => i >= 0);
    let k = 0;
    const days = [];
    play = list.map((it, i) => {
      let ll, interp = false;
      if (hasLoc(it)) ll = L.latLng(it.lat, it.lng);
      else {
        // 依時間在前後兩個有位置的點之間內插
        while (k < known.length && known[k] < i) k++;
        const a = known[k - 1], b = known[k];
        interp = true;
        if (a == null) ll = L.latLng(list[b].lat, list[b].lng);
        else if (b == null) ll = L.latLng(list[a].lat, list[a].lng);
        else {
          const ta = effTime(list[a]), tb = effTime(list[b]);
          const f = tb > ta ? (effTime(it) - ta) / (tb - ta) : 0.5;
          ll = L.latLng(list[a].lat + (list[b].lat - list[a].lat) * f, list[a].lng + (list[b].lng - list[a].lng) * f);
        }
      }
      const day = dayKey(effTime(it));
      if (days[days.length - 1] !== day) days.push(day);
      return { it, ll, interp, day, dayNo: days.length };
    });
  }

  function startRecall() {
    buildPlaylist();
    if (!play.length) return;
    $('pickView').classList.remove('active');
    $('recallView').classList.add('active');
    ensureMap();
    map.invalidateSize();

    const first = effTime(play[0].it), last = effTime(play[play.length - 1].it);
    const nDays = play[play.length - 1].dayNo;
    $('tripTitle').textContent = `${fmtDay(first)} – ${fmtDay(last).slice(5)}　·　${nDays} 天　·　${play.length} 個回憶`;

    routeAll.setLatLngs(play.map(p => p.ll));
    routeDone.setLatLngs([]);
    dotLayer.clearLayers();
    play.forEach((p, i) => {
      p.dot = L.circleMarker(p.ll, { radius: 5, color: '#fff', weight: 2, fillColor: '#8a8f98', fillOpacity: 1 })
        .on('click', () => { stopPlay(); go(i, false); })
        .addTo(dotLayer);
    });
    if (curMarker) { curMarker.remove(); curMarker = null; }

    buildStrip();
    map.fitBounds(routeAll.getBounds(), { ...viewPadding(), maxZoom: 15 });
    cur = -1;
    setTimeout(() => go(0, false), 300);
  }

  function buildStrip() {
    const strip = $('strip');
    const frag = document.createDocumentFragment();
    let lastDay = null;
    play.forEach((p, i) => {
      if (p.day !== lastDay) {
        lastDay = p.day;
        const d = document.createElement('div');
        d.className = 'strip-day';
        d.innerHTML = `<b>D${p.dayNo}</b><span>${fmtDay(effTime(p.it)).slice(5, 10)}</span>`;
        frag.appendChild(d);
      }
      const t = document.createElement('div');
      t.className = 'strip-item' + (p.it.kind === 'video' ? ' video' : '');
      t.innerHTML = `<div class="thumb" data-id="${escapeHtml(p.it.id)}"></div>`;
      t.onclick = () => { stopPlay(); go(i, true); };
      p.stripEl = t;
      frag.appendChild(t);
    });
    strip.replaceChildren(frag);
    strip.querySelectorAll('.thumb').forEach(el => thumbObserver.observe(el));
  }

  // ---------- 移動到第 i 個回憶 ----------
  function go(i, animate = true) {
    if (i < 0 || i >= play.length) return;
    clearTimeout(timer);
    const prev = cur;
    cur = i;
    const p = play[i];
    const token = ++animToken;

    showCard(p);
    updateStripAndDots();
    if (prev < 0 || play[prev].day !== p.day) showDayBanner(p);

    const from = prev >= 0 ? play[prev].ll : null;
    const onArrive = () => {
      if (token !== animToken) return;
      routeDone.setLatLngs(play.slice(0, i + 1).map(q => q.ll));
      placeMarker(p);
      scheduleNext();
    };

    if (!animate || !from || Math.abs(i - prev) !== 1) {
      routeDone.setLatLngs(play.slice(0, i + 1).map(q => q.ll));
      placeMarker(p);
      if (!inView(p.ll)) { const z = Math.max(map.getZoom(), 13); map.flyTo(centerFor(p.ll, z), z, { duration: 0.8 }); }
      scheduleNext();
      return;
    }

    const dist = from.distanceTo(p.ll);
    const speed = parseFloat($('speedSelect').value);
    if (dist < 5) { onArrive(); return; }

    const travel = (ms, done) => animateSegment(from, p.ll, ms / speed, token, i, done);
    if (dist > 30000) {
      // 遠距離：先拉遠看見兩點，畫出路線，再飛到目的地
      map.flyToBounds(L.latLngBounds(from, p.ll), { ...viewPadding(), duration: 1.2 / speed, maxZoom: 13 });
      afterMove(1.2 / speed, () => {
        if (token !== animToken) return;
        travel(1800, () => {
          if (token !== animToken) return;
          map.flyTo(centerFor(p.ll, 14), 14, { duration: 1.2 / speed });
          afterMove(1.2 / speed, onArrive);
        });
      });
    } else {
      if (!inView(p.ll)) {
        const z = map.getZoom() < 12 ? 14 : map.getZoom();
        map.flyTo(centerFor(L.latLngBounds(from, p.ll).getCenter(), z), z, { duration: 0.8 / speed });
      }
      travel(Math.min(1500, 400 + dist / 10), onArrive);
    }
  }

  // 照片卡與下方播放列會蓋住地圖，鏡頭要以「沒被蓋住的區域」為準
  function viewPadding() {
    const size = map.getSize();
    const card = $('card').getBoundingClientRect();
    const top = Math.max(...Array.from(document.querySelectorAll('.recall-top > *'), e => e.getBoundingClientRect().bottom)) + 16;
    const bottom = $('recallView').clientHeight - $('player').getBoundingClientRect().top + 20;
    const sideCard = card.width < size.x * 0.6; // 桌機：卡片在左邊；手機：卡片在下方
    return {
      paddingTopLeft: L.point(sideCard ? card.right + 30 : 30, top),
      paddingBottomRight: L.point(60, sideCard ? bottom : size.y - card.top + 20),
    };
  }
  function visibleRect() {
    const { paddingTopLeft: a, paddingBottomRight: b } = viewPadding();
    const size = map.getSize();
    return L.bounds(a, L.point(size.x - b.x, size.y - b.y));
  }
  function inView(ll) {
    const r = visibleRect();
    const pad = L.point((r.max.x - r.min.x) * 0.12, (r.max.y - r.min.y) * 0.12);
    return L.bounds(r.min.add(pad), r.max.subtract(pad)).contains(map.latLngToContainerPoint(ll));
  }
  // 讓 ll 出現在可見區域的正中央時，地圖中心應該在哪
  function centerFor(ll, zoom) {
    const r = visibleRect(), size = map.getSize();
    const offset = r.getCenter().subtract(size.divideBy(2));
    return map.unproject(map.project(ll, zoom).subtract(offset), zoom);
  }

  // 地圖移動結束後執行；若地圖其實沒動（不會觸發 moveend）就用逾時保底
  function afterMove(sec, fn) {
    let fired = false;
    const run = () => { if (fired) return; fired = true; map.off('moveend', run); fn(); };
    map.once('moveend', run);
    setTimeout(run, sec * 1000 + 400);
  }

  function animateSegment(a, b, ms, token, i, done) {
    const base = play.slice(0, i).map(q => q.ll);
    const t0 = performance.now();
    placeMarker(play[i], a);
    const step = now => {
      if (token !== animToken) return;
      const f = Math.min(1, (now - t0) / ms);
      const e = f < 0.5 ? 2 * f * f : 1 - Math.pow(-2 * f + 2, 2) / 2; // easeInOut
      const ll = L.latLng(a.lat + (b.lat - a.lat) * e, a.lng + (b.lng - a.lng) * e);
      routeDone.setLatLngs(base.concat([ll]));
      curMarker.setLatLng(ll);
      if (f < 1) requestAnimationFrame(step); else done();
    };
    requestAnimationFrame(step);
  }

  function placeMarker(p, at) {
    const html = `<div class="pin${p.it.kind === 'video' ? ' video' : ''}"><div class="thumb" data-id="${escapeHtml(p.it.id)}"></div></div>`;
    const icon = L.divIcon({ className: 'pin-wrap', html, iconSize: [56, 56], iconAnchor: [28, 62] });
    if (!curMarker) curMarker = L.marker(at || p.ll, { icon, zIndexOffset: 1000 }).addTo(map);
    else { curMarker.setLatLng(at || p.ll); if (curMarker._pid !== p.it.id) curMarker.setIcon(icon); }
    if (curMarker._pid !== p.it.id) {
      curMarker._pid = p.it.id;
      const box = curMarker.getElement() && curMarker.getElement().querySelector('.thumb');
      if (box) fillThumb(box, p.it);
    }
  }

  function updateStripAndDots() {
    play.forEach((q, j) => {
      q.stripEl.classList.toggle('active', j === cur);
      q.stripEl.classList.toggle('seen', j < cur);
      q.dot.setStyle({ fillColor: j <= cur ? '#ff6a3d' : '#8a8f98', radius: j === cur ? 7 : 5 });
    });
    play[cur].stripEl.scrollIntoView({ behavior: 'smooth', inline: 'center', block: 'nearest' });
    $('counter').textContent = `${cur + 1} / ${play.length}`;
  }

  // ---------- 照片卡 ----------
  function showCard(p) {
    const it = p.it;
    const box = $('cardMedia');
    box.querySelectorAll('video').forEach(v => v.pause());
    if (it.kind === 'video') {
      const v = document.createElement('video');
      v.src = Media.objectUrl(it);
      v.controls = true; v.playsInline = true;
      v.onended = () => { if (playing && v === currentVideo()) next(); };
      v.onerror = () => { box.innerHTML = '<div class="ph big">🎬<br><small>此瀏覽器無法播放這個影片格式</small></div>'; if (playing) scheduleNext(true); };
      box.replaceChildren(v);
      if (playing) v.play().catch(() => { v.muted = true; v.play().catch(() => {}); });
    } else if (it.renderable) {
      const img = new Image();
      img.src = Media.objectUrl(it);
      box.replaceChildren(img);
    } else {
      // HEIC 等：用內嵌縮圖代替
      const holder = document.createElement('div');
      holder.className = 'thumb';
      box.replaceChildren(holder);
      fillThumb(holder, it);
    }
    $('cardWhen').textContent = `第 ${p.dayNo} 天　${fmtDay(effTime(it))} ${fmtTime(effTime(it))}`;
    $('cardCaption').textContent = it.caption;
    $('cardWhere').textContent = (p.interp ? '約 ' : '') + `${p.ll.lat.toFixed(4)}, ${p.ll.lng.toFixed(4)}`;
    placeName(p);
  }

  function currentVideo() { return $('cardMedia').querySelector('video'); }

  // 反查地名（OpenStreetMap Nominatim，每秒最多一次、有快取）
  const placeCache = new Map();
  let geoTimer = null;
  function placeName(p) {
    clearTimeout(geoTimer);
    const key = `${p.ll.lat.toFixed(3)},${p.ll.lng.toFixed(3)}`;
    const prefix = p.interp ? '約在 ' : '';
    if (placeCache.has(key)) { if (placeCache.get(key)) $('cardWhere').textContent = prefix + placeCache.get(key); return; }
    const want = p;
    geoTimer = setTimeout(async () => {
      try {
        const r = await fetch(`https://nominatim.openstreetmap.org/reverse?format=json&zoom=16&lat=${p.ll.lat}&lon=${p.ll.lng}`,
          { headers: { 'Accept-Language': 'zh-TW,zh,en' } });
        const j = await r.json();
        const a = j.address || {};
        const name = [a.attraction || a.tourism || a.amenity || a.building || a.road,
          a.suburb || a.neighbourhood || a.quarter, a.city || a.town || a.village || a.county, a.country]
          .filter(Boolean).join('，');
        placeCache.set(key, name || '');
        if (name && play[cur] === want) $('cardWhere').textContent = prefix + name;
      } catch (e) { placeCache.set(key, ''); }
    }, 1100);
  }

  function showDayBanner(p) {
    const b = $('dayBanner');
    b.innerHTML = `<div>第 ${p.dayNo} 天</div><small>${fmtDay(effTime(p.it))}</small>`;
    b.classList.remove('show');
    void b.offsetWidth;
    b.classList.add('show');
  }

  // ---------- 播放控制 ----------
  function scheduleNext(force) {
    clearTimeout(timer);
    if (!playing) return;
    const speed = parseFloat($('speedSelect').value);
    const it = play[cur].it;
    if (it.kind === 'video' && !force) {
      const v = currentVideo();
      if (v) {
        if (v.paused) v.play().catch(() => { v.muted = true; v.play().catch(() => {}); });
        if ($('fullVideoChk').checked) return; // 等 onended
        timer = setTimeout(next, 8000 / speed);
        return;
      }
    }
    timer = setTimeout(next, 3500 / speed);
  }

  function next() {
    if (cur < play.length - 1) go(cur + 1, true);
    else {
      stopPlay();
      const b = $('dayBanner');
      b.innerHTML = '<div>旅程結束</div><small>謝謝這段回憶 ✨</small>';
      b.classList.remove('show'); void b.offsetWidth; b.classList.add('show');
      map.flyToBounds(routeAll.getBounds(), { ...viewPadding(), duration: 1.5, maxZoom: 15 });
    }
  }

  function startPlay() {
    playing = true;
    $('playBtn').textContent = '⏸';
    if (cur >= play.length - 1) go(0, false);
    else scheduleNext();
  }
  function stopPlay() {
    playing = false;
    clearTimeout(timer);
    $('playBtn').textContent = '▶';
  }

  $('playBtn').onclick = () => (playing ? stopPlay() : startPlay());
  $('prevBtn').onclick = () => { stopPlay(); go(cur - 1, false); };
  $('nextBtn').onclick = () => { stopPlay(); go(cur + 1, true); };
  $('startBtn').onclick = startRecall;
  $('backBtn').onclick = () => {
    stopPlay();
    animToken++;
    const v = currentVideo(); if (v) v.pause();
    $('recallView').classList.remove('active');
    $('pickView').classList.add('active');
  };

  document.addEventListener('keydown', e => {
    if (!$('recallView').classList.contains('active')) return;
    if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
    if (e.key === ' ') { e.preventDefault(); playing ? stopPlay() : startPlay(); }
    else if (e.key === 'ArrowRight') { stopPlay(); go(cur + 1, true); }
    else if (e.key === 'ArrowLeft') { stopPlay(); go(cur - 1, false); }
    else if (e.key === 'Escape') $('backBtn').click();
  });
})();
