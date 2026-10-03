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
        const d = t.DateTimeOriginal || t.CreateDate || t.ModifyDate;
        if (d instanceof Date && !isNaN(d)) { out.time = d.getTime(); out.timeSource = 'exif'; }
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
          time: meta.time ?? file.lastModified,
          timeSource: meta.timeSource || 'file',
          orientation: meta.orientation || 1,
          lat: meta.lat ?? null,
          lng: meta.lng ?? null,
          locSource: meta.lat != null ? 'gps' : null,
          selected: true,
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

  // 優先用 EXIF 內嵌縮圖（快很多），沒有才用原圖
  async function thumbUrl(item) {
    if (item.thumbUrl) return item.thumbUrl;
    if (item.kind === 'image') {
      try {
        const u = await exifr.thumbnailUrl(item.file);
        if (u) { item.thumbUrl = u; item.thumbIsExif = true; return u; }
      } catch (e) { /* ignore */ }
    }
    item.thumbUrl = objectUrl(item);
    return item.thumbUrl;
  }

  // EXIF 內嵌縮圖不會自動轉正，依 Orientation 補旋轉角度
  function thumbRotation(item) {
    if (!item.thumbIsExif) return 0;
    return ({ 3: 180, 6: 90, 8: 270 })[item.orientation] || 0;
  }

  window.Media = { loadFiles, objectUrl, thumbUrl, thumbRotation, validLatLng };
})();
