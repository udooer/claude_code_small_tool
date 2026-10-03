/* 讀取照片 / 影片的拍攝時間與 GPS 位置。
 *
 * 時間一律存成「當地牆上時間」：Date 物件在瀏覽器本地時區顯示出來的值，
 * 就等於拍攝當地的時鐘時間。這樣照片（EXIF 沒有時區）和影片才能放在同一條時間軸上。
 */
(function () {
  'use strict';

  const IMAGE_EXT = ['jpg', 'jpeg', 'png', 'webp', 'heic', 'heif', 'gif', 'avif', 'tif', 'tiff'];
  const VIDEO_EXT = ['mp4', 'mov', 'm4v', '3gp', 'webm'];
  // 瀏覽器（Safari 以外）無法直接顯示的格式
  const UNRENDERABLE = ['heic', 'heif', 'tif', 'tiff'];

  function ext(name) {
    const i = name.lastIndexOf('.');
    return i < 0 ? '' : name.slice(i + 1).toLowerCase();
  }

  function kindOf(file) {
    const e = ext(file.name);
    if (VIDEO_EXT.includes(e) || file.type.startsWith('video/')) return 'video';
    if (IMAGE_EXT.includes(e) || file.type.startsWith('image/')) return 'image';
    return null;
  }

  function fileKey(file) {
    return `${file.webkitRelativePath || file.name}|${file.size}`;
  }

  function validLatLng(lat, lng) {
    return Number.isFinite(lat) && Number.isFinite(lng) &&
      Math.abs(lat) <= 90 && Math.abs(lng) <= 180 && !(lat === 0 && lng === 0);
  }

  // ---------- 時間解析 ----------
  // EXIF / XMP 的日期（Date 或 "2025:12:24 15:43:12" / ISO 字串）→ 牆上時間 ms
  function toTime(v) {
    if (v instanceof Date) return isNaN(v) ? null : v.getTime();
    if (typeof v !== 'string') return null;
    const m = v.match(/^(\d{4})[:-](\d{2})[:-](\d{2})(?:[ T](\d{2}):(\d{2})(?::(\d{2}))?)?/);
    return m ? wallTime(+m[1], +m[2], +m[3], +(m[4] || 12), +(m[5] || 0), +(m[6] || 0)) : null;
  }

  function wallTime(Y, M, D, h = 12, m = 0, s = 0) {
    if (Y < 1990 || Y > 2100 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || s > 59) return null;
    const d = new Date(Y, M - 1, D, h, m, s);
    return d.getMonth() === M - 1 ? d.getTime() : null; // 擋掉 2/31 之類
  }

  // 從檔名猜拍攝時間，例如：
  //   IMG_20251224_154312.jpg、PXL_20251224_064312345.jpg、20251224_154312.mp4
  //   Screenshot_2025-12-24-15-43-12.png、Photo 2025-12-24 15 43 12.jpg
  //   IMG-20251224-WA0001.jpg（只有日期）、1766562192000.jpg（Unix 時間戳）
  function parseNameTime(name) {
    const base = name.replace(/\.[^.]+$/, '');
    let m = base.match(/(?:^|\D)(20\d{2})[-_.]?(\d{2})[-_.]?(\d{2})[ _T.-]?(\d{2})[-_.:h ]?(\d{2})[-_.:m ]?(\d{2})/);
    if (m) {
      const t = wallTime(+m[1], +m[2], +m[3], +m[4], +m[5], +m[6]);
      if (t != null) return { time: t, source: 'name' };
    }
    m = base.match(/(?:^|\D)(1[5-9]\d{8})(\d{3})?(?:\D|$)/); // 秒或毫秒時間戳（2017–2033 年）
    if (m) return { time: (+m[1]) * 1000, source: 'name' };
    m = base.match(/(?:^|\D)(20\d{2})[-_.]?(\d{2})[-_.]?(\d{2})(?:\D|$)/);
    if (m) {
      const t = wallTime(+m[1], +m[2], +m[3]);
      if (t != null) return { time: t, source: 'name-date' };
    }
    return null;
  }

  // ---------- 照片：EXIF ----------
  async function readImage(file) {
    const out = {};
    try {
      const t = await exifr.parse(file, {
        tiff: true, exif: true, gps: true, ifd1: false, xmp: true,
        icc: false, iptc: false, interop: false, mergeOutput: true,
        reviveValues: true,
      });
      if (t) {
        // 拍攝時間：DateTimeOriginal 最準；ModifyDate 可能是後製時間，只當備案
        const shot = [t.DateTimeOriginal, t.DateTimeDigitized, t.CreateDate, t.DateCreated].map(toTime).find(v => v != null);
        const mod = toTime(t.ModifyDate);
        if (shot != null) { out.time = shot; out.timeSource = 'exif'; }
        else if (mod != null) { out.time = mod; out.timeSource = 'exif-modify'; }
        let lat = t.latitude, lng = t.longitude;
        if (!Number.isFinite(lat) && Array.isArray(t.GPSLatitude) && Array.isArray(t.GPSLongitude)) {
          const dms = a => a[0] + (a[1] || 0) / 60 + (a[2] || 0) / 3600;
          lat = dms(t.GPSLatitude) * (t.GPSLatitudeRef === 'S' ? -1 : 1);
          lng = dms(t.GPSLongitude) * (t.GPSLongitudeRef === 'W' ? -1 : 1);
        }
        if (validLatLng(lat, lng)) { out.lat = lat; out.lng = lng; }
        out.orientation = typeof t.Orientation === 'number' ? t.Orientation
          : ({ 'Rotate 90 CW': 6, 'Rotate 180': 3, 'Rotate 270 CW': 8 })[t.Orientation] || 1;
      }
    } catch (e) { /* 沒有 EXIF（PNG、截圖等）就用檔案時間 */ }
    return out;
  }

  // ---------- 影片：MP4 / MOV 的 moov box ----------
  async function readBytes(file, start, len) {
    return new Uint8Array(await file.slice(start, start + len).arrayBuffer());
  }

  async function findMoov(file) {
    let pos = 0;
    for (let guard = 0; guard < 64 && pos + 8 <= file.size; guard++) {
      const h = await readBytes(file, pos, 16);
      const dv = new DataView(h.buffer);
      let size = dv.getUint32(0);
      const type = String.fromCharCode(h[4], h[5], h[6], h[7]);
      let header = 8;
      if (size === 1) { size = dv.getUint32(8) * 2 ** 32 + dv.getUint32(12); header = 16; }
      else if (size === 0) size = file.size - pos;
      if (size < header) return null;
      if (type === 'moov') return { pos, size };
      pos += size;
    }
    return null;
  }

  function parseMvhd(bytes) {
    // 在 moov 的子 box 中找 mvhd
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let p = 8;
    while (p + 8 <= bytes.length) {
      const size = dv.getUint32(p);
      const type = String.fromCharCode(bytes[p + 4], bytes[p + 5], bytes[p + 6], bytes[p + 7]);
      if (type === 'mvhd') {
        const version = bytes[p + 8];
        const secs = version === 1
          ? dv.getUint32(p + 12) * 2 ** 32 + dv.getUint32(p + 16)
          : dv.getUint32(p + 12);
        if (secs > 0) return (secs - 2082844800) * 1000; // 1904-01-01 → 1970-01-01
        return null;
      }
      if (size < 8) break;
      p += size;
    }
    return null;
  }

  async function readVideo(file) {
    const out = {};
    try {
      const moov = await findMoov(file);
      if (!moov || moov.size > 64 * 1024 * 1024) return out;
      const bytes = await readBytes(file, moov.pos, moov.size);
      let text = '';
      for (let i = 0; i < bytes.length; i += 8192) {
        text += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
      }

      // Apple: com.apple.quicktime.creationdate = "2025-05-03T14:22:10+0900"（含時區，最準）
      const cd = text.match(/(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:[+-]\d{2}:?\d{2}|Z)/);
      if (cd) {
        const [, Y, M, D, h, m, s] = cd.map(Number);
        out.time = new Date(Y, M - 1, D, h, m, s).getTime(); // 牆上時間
        out.timeSource = 'video-local';
      } else {
        const utc = parseMvhd(bytes);
        if (utc && utc > Date.UTC(1990, 0, 1)) {
          out.time = utc;               // UTC → 以瀏覽器時區顯示，可用「影片時間校正」修正
          out.timeSource = 'video-utc';
        }
      }

      // ISO 6709：©xyz 或 com.apple.quicktime.location.ISO6709，例如 "+35.0036+135.7681+012.000/"
      const loc = text.match(/([+-]\d{1,2}\.\d{3,})([+-]\d{1,3}\.\d{3,})/);
      if (loc) {
        const lat = parseFloat(loc[1]), lng = parseFloat(loc[2]);
        if (validLatLng(lat, lng)) { out.lat = lat; out.lng = lng; }
      }
    } catch (e) { /* 讀不到就算了 */ }
    return out;
  }

  function timeFields(meta, name) {
    if (meta.time != null) return { time: meta.time, timeSource: meta.timeSource };
    const n = parseNameTime(name);
    return n ? { time: n.time, timeSource: n.source } : { time: null, timeSource: 'none' };
  }

  // ---------- 對外 ----------
  async function loadFiles(fileList, onProgress) {
    const files = Array.from(fileList).filter(f => kindOf(f) && !f.name.startsWith('._'));
    const items = new Array(files.length);
    let done = 0, next = 0;

    async function worker() {
      while (next < files.length) {
        const i = next++;
        const file = files[i];
        const kind = kindOf(file);
        const meta = kind === 'video' ? await readVideo(file) : await readImage(file);
        items[i] = {
          id: fileKey(file),
          key: fileKey(file),
          file,
          name: file.name,
          kind,
          renderable: !UNRENDERABLE.includes(ext(file.name)),
          // 檔案修改時間常是複製 / 下載的時間，不可靠，所以不用
          ...timeFields(meta, file.name),
          orientation: meta.orientation || 1,
          lat: meta.lat ?? null,
          lng: meta.lng ?? null,
          locSource: meta.lat != null ? 'gps' : null,
          selected: meta.time != null || !!parseNameTime(file.name),
          caption: '',
          url: null,
          thumbUrl: null,
        };
        done++;
        onProgress && onProgress(done, files.length);
      }
    }
    await Promise.all(Array.from({ length: Math.min(6, files.length) }, worker));
    return items;
  }

  function objectUrl(item) {
    if (!item.url) item.url = URL.createObjectURL(item.file);
    return item.url;
  }

  // ---------- HEIC：Chrome / Edge 不能直接顯示，用 libheif（heic-to）在瀏覽器裡解碼 ----------
  let heicLib = null;      // 載入 lib/heic-to 的 Promise（約 3 MB，有 HEIC 才載入）
  let nativeHeic = null;   // 瀏覽器本身能不能顯示 HEIC（Safari 可以）
  const heicQueue = [];    // 解碼工作佇列（heic-to 只有一個 worker，一次一張）
  let heicBusy = false;

  function loadHeicLib() {
    if (!heicLib) {
      heicLib = new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = 'lib/heic-to/heic-to.js';
        s.onload = () => (window.HeicTo ? resolve(window.HeicTo) : reject(new Error('heic-to 載入失敗')));
        s.onerror = () => reject(new Error('找不到 lib/heic-to/heic-to.js'));
        document.head.appendChild(s);
      });
    }
    return heicLib;
  }

  function canDecodeNatively(item) {
    if (nativeHeic === null) {
      const img = new Image();
      img.src = objectUrl(item);
      nativeHeic = img.decode().then(() => true, () => false);
    }
    return nativeHeic;
  }

  // 解碼並縮成最長邊 maxSide 的 JPEG，回傳 object URL；urgent 會插隊（播放時正在看的那張）
  function heicToJpegUrl(item, maxSide, urgent) {
    return new Promise((resolve, reject) => {
      const job = { item, maxSide, resolve, reject };
      urgent ? heicQueue.unshift(job) : heicQueue.push(job);
      pumpHeic();
    });
  }

  async function pumpHeic() {
    if (heicBusy || !heicQueue.length) return;
    heicBusy = true;
    const job = heicQueue.shift();
    try {
      const heicTo = await loadHeicLib();
      const bmp = await heicTo({ blob: job.item.file, type: 'bitmap' });
      const k = Math.min(1, job.maxSide / Math.max(bmp.width, bmp.height));
      const c = document.createElement('canvas');
      c.width = Math.round(bmp.width * k);
      c.height = Math.round(bmp.height * k);
      c.getContext('2d').drawImage(bmp, 0, 0, c.width, c.height);
      bmp.close && bmp.close();
      const blob = await new Promise(r => c.toBlob(r, 'image/jpeg', 0.88));
      c.width = c.height = 1;
      job.resolve(URL.createObjectURL(blob));
    } catch (e) {
      job.reject(e);
    } finally {
      heicBusy = false;
      pumpHeic();
    }
  }

  // 縮圖：一般照片優先用 EXIF 內嵌縮圖（快很多），HEIC 解碼成小張 JPEG
  async function thumbUrl(item) {
    if (item.thumbUrl) return item.thumbUrl;
    if (!item.thumbPromise) item.thumbPromise = (async () => {
      if (item.kind === 'image' && item.renderable) {
        try {
          const u = await exifr.thumbnailUrl(item.file);
          if (u) { item.thumbIsExif = true; return u; }
        } catch (e) { /* ignore */ }
      }
      if (item.kind === 'image' && !item.renderable) {
        if (await canDecodeNatively(item)) return objectUrl(item);
        return heicToJpegUrl(item, 400, false);
      }
      return objectUrl(item);
    })();
    try {
      item.thumbUrl = await item.thumbPromise;
    } catch (e) {
      item.thumbPromise = null;
      throw e;
    }
    return item.thumbUrl;
  }

  // 大圖：HEIC 解碼成 2048px JPEG，只保留最近幾張以免吃光記憶體
  const displayCache = new Map();
  async function displayUrl(item) {
    if (item.renderable || item.kind !== 'image' || await canDecodeNatively(item)) return objectUrl(item);
    if (displayCache.has(item.id)) return displayCache.get(item.id);
    const p = heicToJpegUrl(item, 2048, true);
    displayCache.set(item.id, p);
    p.catch(() => displayCache.delete(item.id));
    while (displayCache.size > 8) {
      const [oldId, oldP] = displayCache.entries().next().value;
      displayCache.delete(oldId);
      oldP.then(u => URL.revokeObjectURL(u), () => {});
    }
    return p;
  }

  // EXIF 內嵌縮圖不會自動轉正，依 Orientation 補旋轉角度（HEIC 解碼後已轉正）
  function thumbRotation(item) {
    if (!item.thumbIsExif) return 0;
    return ({ 3: 180, 6: 90, 8: 270 })[item.orientation] || 0;
  }

  window.Media = { loadFiles, parseNameTime, wallTime, objectUrl, thumbUrl, displayUrl, thumbRotation, validLatLng };
})();
