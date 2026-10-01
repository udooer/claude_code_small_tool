#!/usr/bin/env python3
"""trip.json -> 單檔靜態網頁 (手機友善、可離線、無外部相依)。

用法:
  python build.py trip.json -o dist            # 產生 dist/index.html
  python build.py trip.json -o dist --slug     # 產生 dist/<隨機碼>/index.html (不易被猜到的網址)
  python build.py trip.json -o dist --password "密碼"   # 內容用 AES-256-GCM 加密, 開啟時需輸入密碼
                                                       # (需要 pip install cryptography)
"""
import argparse, base64, hashlib, html, json, os, re, secrets, sys, urllib.parse
from pathlib import Path

E = html.escape
TYPES = {"transport": ("🚆", "交通"), "sight": ("📍", "景點"), "food": ("🍜", "餐食"),
         "lodging": ("🏨", "住宿"), "other": ("•", "其他")}


def maps_url(q):
    return "https://www.google.com/maps/search/?api=1&query=" + urllib.parse.quote(q)


def link(href, text, cls="btn"):
    return f'<a class="{cls}" href="{E(href, True)}" target="_blank" rel="noopener">{E(text)}</a>'


def tel(num):
    return "tel:" + re.sub(r"[^\d+]", "", num)


def has_icon(text):
    c = ord(text[0])
    return 0x2190 <= c < 0x2C00 or c >= 0x1F000


def render_item(it, city=""):
    icon, label = TYPES.get(it.get("type", "other"), TYPES["other"])
    q = it.get("address") or (it.get("title", "") + " " + city).strip()
    actions = []
    if it.get("map") or it.get("address") or it.get("type") in ("sight", "food", "lodging"):
        actions.append(link(it.get("map") or maps_url(q), "地圖"))
    if it.get("url"):
        actions.append(link(it["url"], "網站"))
    meta = []
    if it.get("booking"):
        meta.append(f'<span>🎫 {E(it["booking"])}</span>')
    if it.get("cost"):
        meta.append(f'<span>💴 {E(it["cost"])}</span>')
    return (
        f'<li class="item t-{E(it.get("type", "other"))}"><div class="time">{E(it.get("time", ""))}</div>'
        f'<div class="body"><div class="ttl">{icon} {E(it.get("title", ""))}</div>'
        + (f'<div class="det">{E(it["detail"])}</div>' if it.get("detail") else "")
        + (f'<div class="det addr">{E(it["address"])}</div>' if it.get("address") else "")
        + (f'<div class="det note">{"" if has_icon(it["note"]) else "💡 "}{E(it["note"])}</div>' if it.get("note") else "")
        + (f'<div class="meta">{"".join(meta)}</div>' if meta else "")
        + (f'<div class="acts">{"".join(actions)}</div>' if actions else "")
        + "</div></li>"
    )


def render_lodging(l):
    if not l:
        return ""
    acts = [link(l.get("map") or maps_url(l.get("address") or l["name"]), "地圖")]
    if l.get("phone"):
        acts.append(link(tel(l["phone"]), "撥打"))
    rows = [f'<div class="ttl">🏨 {E(l["name"])}</div>']
    for k, lab in (("address", "地址"), ("checkin", "入住"), ("checkout", "退房"), ("booking", "訂位")):
        if l.get(k):
            rows.append(f'<div class="det">{lab}：{E(l[k])}</div>')
    return f'<div class="card lodging">{"".join(rows)}<div class="acts">{"".join(acts)}</div></div>'


def render_day(i, d):
    notes = "".join(f"<li>{E(n)}</li>" for n in d.get("notes", []))
    return (
        f'<section class="day" id="d{i}" data-date="{E(d.get("date", ""))}">'
        f'<h2>Day {i + 1} · {E(d.get("date", ""))}<small>{E(d.get("title", ""))}</small></h2>'
        + (f'<div class="card hl">⭐ {E(d["highlight"])}</div>' if d.get("highlight") else "")
        + (f'<div class="card wx">🌤 {E(d["weather"])}</div>' if d.get("weather") else "")
        + render_lodging(d.get("lodging"))
        + f'<ul class="items">{"".join(render_item(x, d.get("city", "")) for x in d.get("items", []))}</ul>'
        + (f'<div class="card warn"><b>⚠️ 注意事項</b><ul>{notes}</ul></div>' if notes else "")
        + "</section>"
    )


def render_overview(trip):
    ov = trip.get("overview", {})
    parts = []
    if ov.get("notes"):
        parts.append('<div class="card warn"><b>⚠️ 行前重點</b><ul>' + "".join(f"<li>{E(n)}</li>" for n in ov["notes"]) + "</ul></div>")
    for sec in ov.get("sections", []):
        cls = "card warn" if sec.get("warn") else "card"
        rows = "".join(f"<li>{E(x)}</li>" for x in sec.get("items", []))
        parts.append(f'<div class="{cls}"><b>{E(sec["title"])}</b><ul>{rows}</ul></div>')
    if ov.get("contacts"):
        rows = "".join(
            f'<li>{E(c["label"])}：<a href="{E(tel(c["value"]), True)}">{E(c["value"])}</a></li>' for c in ov["contacts"])
        parts.append(f'<div class="card"><b>📞 緊急聯絡</b><ul>{rows}</ul></div>')
    if trip.get("checklist"):
        rows = "".join(
            f'<li><label><input type="checkbox" data-k="c{j}"> {E(c)}</label></li>' for j, c in enumerate(trip["checklist"]))
        parts.append(f'<div class="card"><b>🎒 行李清單</b><ul class="chk">{rows}</ul></div>')
    return f'<section class="day" id="ov"><h2>總覽</h2>{"".join(parts)}</section>'


CSS = """
:root{--bg:#f6f7f9;--fg:#1b1f24;--card:#fff;--mut:#667;--ac:#2563eb;--wr:#fff4e0;--hl:#e8f0ff;--bd:#e3e6ea}
@media(prefers-color-scheme:dark){:root{--bg:#111418;--fg:#e8eaed;--card:#1b1f26;--mut:#9aa3ad;--ac:#7aa7ff;--wr:#3a2e18;--hl:#1b2a45;--bd:#2a3039}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,"Noto Sans TC",sans-serif}
header{padding:16px 16px 8px}header h1{margin:0;font-size:1.4rem}header p{margin:0;color:var(--mut)}
nav{position:sticky;top:0;z-index:5;display:flex;gap:6px;overflow-x:auto;padding:8px 12px;background:var(--bg);border-bottom:1px solid var(--bd)}
nav button{flex:none;border:1px solid var(--bd);background:var(--card);color:var(--fg);border-radius:999px;padding:6px 14px;font-size:.95rem}
nav button.on{background:var(--ac);color:#fff;border-color:var(--ac)}
main{padding:12px 16px 48px;max-width:720px;margin:auto}
.js .day{display:none}.js .day.on{display:block}
h2{font-size:1.15rem;margin:8px 0 12px}h2 small{display:block;color:var(--mut);font-weight:400;font-size:.95rem}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:10px 14px;margin:10px 0}
.hl{background:var(--hl)}.warn{background:var(--wr)}.card ul{margin:6px 0 0;padding-left:20px}.chk{list-style:none;padding:0!important}
.items{list-style:none;padding:0;margin:12px 0}
.item{display:flex;gap:12px;background:var(--card);border:1px solid var(--bd);border-left:4px solid var(--ac);border-radius:12px;padding:10px 12px;margin:10px 0}
.t-transport{border-left-color:#d97706}.t-food{border-left-color:#16a34a}.t-lodging{border-left-color:#9333ea}
.time:empty{display:none}.time{flex:none;width:3.6em;font-size:.9rem;overflow-wrap:anywhere;font-variant-numeric:tabular-nums;color:var(--mut);font-weight:600}
.ttl{font-weight:600}.det{color:var(--mut);font-size:.92rem;white-space:pre-line;overflow-wrap:anywhere}.meta{font-size:.9rem;display:flex;gap:12px;flex-wrap:wrap;margin-top:2px}
.acts{display:flex;gap:8px;margin-top:6px}.btn{font-size:.85rem;padding:3px 12px;border-radius:999px;border:1px solid var(--ac);color:var(--ac);text-decoration:none}
a{color:var(--ac)}footer{text-align:center;color:var(--mut);font-size:.8rem;padding:16px}
@media print{nav{display:none}.js .day{display:block}}
"""

JS = """
(function(){document.documentElement.classList.add('js');
var secs=[].slice.call(document.querySelectorAll('.day')),nav=document.querySelector('nav');
function show(id){secs.forEach(function(s){s.classList.toggle('on',s.id===id)});
[].forEach.call(nav.children,function(b){b.classList.toggle('on',b.dataset.t===id)});window.scrollTo(0,0);
try{localStorage.setItem('tab',id)}catch(e){}}
secs.forEach(function(s){var b=document.createElement('button');b.dataset.t=s.id;
b.textContent=s.id==='ov'?'總覽':(s.dataset.date||s.id).slice(5)||s.id;b.onclick=function(){show(s.id)};nav.appendChild(b)});
var t=new Date(),p=function(n){return(n<10?'0':'')+n},today=t.getFullYear()+'-'+p(t.getMonth()+1)+'-'+p(t.getDate());
var cur=secs.filter(function(s){return s.dataset.date===today})[0],saved;
try{saved=localStorage.getItem('tab')}catch(e){}
show(cur?cur.id:(saved&&document.getElementById(saved)?saved:(secs[1]||secs[0]).id));
[].forEach.call(document.querySelectorAll('input[data-k]'),function(i){var k='k_'+i.dataset.k;
try{i.checked=localStorage.getItem(k)==='1'}catch(e){}i.onchange=function(){try{localStorage.setItem(k,i.checked?'1':'0')}catch(e){}}});
})();
"""


def build(trip):
    days = "".join(render_day(i, d) for i, d in enumerate(trip.get("days", [])))
    return f"""<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex,nofollow">
<title>{E(trip.get("title", "旅遊行程"))}</title><style>{CSS}</style></head><body>
<header><h1>{E(trip.get("title", ""))}</h1><p>{E(trip.get("subtitle", ""))}</p></header><nav></nav>
<main>{render_overview(trip)}{days}</main><footer>離線也可開啟 · 加入手機主畫面更方便</footer>
<script>{JS}</script></body></html>"""


ITER = 600_000

LOCK_JS = """
(async function(){
var D=%s,b=function(s){return Uint8Array.from(atob(s),function(c){return c.charCodeAt(0)})};
var f=document.getElementById('f'),pw=document.getElementById('pw'),msg=document.getElementById('m');
async function open_(p){var k=await crypto.subtle.importKey('raw',new TextEncoder().encode(p),'PBKDF2',false,['deriveKey']);
var key=await crypto.subtle.deriveKey({name:'PBKDF2',salt:b(D.s),iterations:D.n,hash:'SHA-256'},k,{name:'AES-GCM',length:256},false,['decrypt']);
var t=await crypto.subtle.decrypt({name:'AES-GCM',iv:b(D.i)},key,b(D.c));return new TextDecoder().decode(t)}
async function go(p,auto){msg.textContent='解鎖中…';
try{var h=await open_(p);try{localStorage.setItem('tp_pw_'+D.s,p)}catch(e){}document.open();document.write(h);document.close()}
catch(e){msg.textContent=auto?'':'密碼錯誤';if(auto){try{localStorage.removeItem('tp_pw_'+D.s)}catch(e){}}}}
f.onsubmit=function(e){e.preventDefault();go(pw.value,false)};
var saved;try{saved=localStorage.getItem('tp_pw_'+D.s)}catch(e){}
if(saved)go(saved,true);else msg.textContent='';
})();
"""


def encrypt_page(inner_html, password, title="旅遊行程"):
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        sys.exit("--password 需要 cryptography 套件: pip install cryptography")
    salt, iv = os.urandom(16), os.urandom(12)
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, ITER, dklen=32)
    ct = AESGCM(key).encrypt(iv, inner_html.encode(), None)
    b64 = lambda x: base64.b64encode(x).decode()
    data = json.dumps({"s": b64(salt), "i": b64(iv), "c": b64(ct), "n": ITER})
    return f"""<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex,nofollow">
<title>{E(title)}</title><style>
body{{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;font:16px -apple-system,"Noto Sans TC",sans-serif;background:#f6f7f9;color:#1b1f24}}
@media(prefers-color-scheme:dark){{body{{background:#111418;color:#e8eaed}}}}
form{{width:min(320px,86vw);text-align:center}}input,button{{width:100%;padding:12px;margin-top:10px;font-size:1rem;border-radius:10px;border:1px solid #8896;box-sizing:border-box}}
button{{background:#2563eb;color:#fff;border:0}}#m{{color:#d33;min-height:1.5em;margin-top:8px}}</style></head><body>
<form id="f"><div style="font-size:2rem">🔒</div><p>請輸入密碼</p>
<input id="pw" type="password" autocomplete="current-password" autofocus><button>解鎖</button><div id="m"></div></form>
<script>{LOCK_JS % data}</script></body></html>"""


def validate(trip):
    errs = []
    if not trip.get("days"):
        errs.append("缺少 days")
    for i, d in enumerate(trip.get("days", [])):
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", d.get("date", "")):
            errs.append(f"days[{i}].date 需為 YYYY-MM-DD")
        for j, it in enumerate(d.get("items", [])):
            if not it.get("title"):
                errs.append(f"days[{i}].items[{j}] 缺少 title")
            if it.get("type", "other") not in TYPES:
                errs.append(f"days[{i}].items[{j}].type 無效: {it.get('type')}")
    return errs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trip"), ap.add_argument("-o", "--out", default="dist")
    ap.add_argument("--slug", action="store_true", help="輸出到隨機子目錄")
    ap.add_argument("--password", help="以此密碼加密整個頁面")
    a = ap.parse_args()
    trip = json.loads(Path(a.trip).read_text(encoding="utf-8"))
    errs = validate(trip)
    if errs:
        sys.exit("行程資料有問題:\n  " + "\n  ".join(errs))
    out = Path(a.out)
    if a.slug:
        out = out / secrets.token_urlsafe(8).replace("_", "x").replace("-", "y")
    out.mkdir(parents=True, exist_ok=True)
    page = build(trip)
    if a.password:
        if len(a.password) < 8:
            sys.exit("密碼至少 8 個字元 (建議 4 個英文單字以上)")
        page = encrypt_page(page, a.password)
    (out / "index.html").write_text(page, encoding="utf-8")
    print(f"OK -> {out / 'index.html'}")


if __name__ == "__main__":
    main()
