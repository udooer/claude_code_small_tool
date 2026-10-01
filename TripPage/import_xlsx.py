#!/usr/bin/env python3
"""把「每天一個分頁」的 Google Sheet (下載成 .xlsx) 轉成 trip.json。純標準函式庫。

分頁格式: 名稱如 "10/9 (Day 1) 維也納"; 欄位: 時間 | 行程與景點 | 交通與行李 | 餐食與住宿 | 備註與拍攝點
其他分頁 (總表、含「交通」「訂票」字樣) 會整理進 overview.notes; 名稱不含 "Day" 的分頁若無法辨識會被略過。

  python import_xlsx.py trip.xlsx --year 2026 --title "東歐 18 天" -o trips/eu-xxxx.json
"""
import argparse, json, re, sys, zipfile
import xml.etree.ElementTree as ET

NS = {"m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
      "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships"}
FOOD = "☕🍴🍽🍷🍹🍺🥐🍰"
TRANSPORT_WORDS = ("前往", "移動", "返回", "回維也納", "班機", "搭機", "出發")


def col_idx(ref):
    n = 0
    for ch in re.match(r"[A-Z]+", ref).group():
        n = n * 26 + ord(ch) - 64
    return n - 1


def read_xlsx(path):
    z = zipfile.ZipFile(path)
    sst = []
    if "xl/sharedStrings.xml" in z.namelist():
        for si in ET.fromstring(z.read("xl/sharedStrings.xml")).findall("m:si", NS):
            sst.append("".join(t.text or "" for t in si.iter("{%s}t" % NS["m"])))
    wb = ET.fromstring(z.read("xl/workbook.xml"))
    rels = {r.get("Id"): r.get("Target") for r in ET.fromstring(z.read("xl/_rels/workbook.xml.rels"))}
    sheets = []
    for sh in wb.find("m:sheets", NS):
        target = rels[sh.get("{%s}id" % NS["r"])].lstrip("/")
        target = target if target.startswith("xl/") else "xl/" + target
        rows = []
        for row in ET.fromstring(z.read(target)).iter("{%s}row" % NS["m"]):
            cells = {}
            for c in row.findall("m:c", NS):
                t, v = c.get("t"), c.find("m:v", NS)
                if t == "inlineStr":
                    val = "".join(x.text or "" for x in c.iter("{%s}t" % NS["m"]))
                elif v is None:
                    continue
                else:
                    val = sst[int(v.text)] if t == "s" else v.text
                cells[col_idx(c.get("r"))] = val.strip()
            if any(cells.values()):
                rows.append([cells.get(i, "") for i in range(max(cells) + 1)])
        sheets.append((sh.get("name"), rows))
    return sheets


def parse_lodging(text):
    name, _, addr = text.replace("🏨", "").partition("📍")
    d = {"name": name.strip()}
    if addr.strip():
        d["address"] = addr.strip()
    return d


def classify(title, c, d):
    if title.startswith("住宿"):
        return "lodging"
    if any(w in title for w in TRANSPORT_WORDS):
        return "transport"
    if (d and d[0] in FOOD) or any(w in title for w in ("咖啡", "午餐", "晚餐", "早餐")):
        return "food"
    return "sight"


def day_sheet(name, rows, year):
    m = re.match(r"\s*(\d{1,2})/(\d{1,2})\s*(?:\(Day\s*\d+\))?\s*(.*)", name)
    day = {"date": f"{year}-{int(m.group(1)):02d}-{int(m.group(2)):02d}", "title": m.group(3).strip(), "items": [], "notes": []}
    for r in rows:
        r += [""] * (5 - len(r))
        t, title, c, d, e = r[:5]
        if t == "時間":
            continue
        if d.startswith("🏨") and not day.get("lodging"):
            day["lodging"] = parse_lodging(d)
            d = ""
        if not (title or c or d or e):
            continue
        if not title:  # 例如只有「紫色實體交通卡!!」的提醒列
            day["notes"] += [x for x in (c, e) if x]
            continue
        item = {"time": t, "type": classify(title, c, d), "title": title}
        if c:
            item["detail"] = c
        if d:
            item["detail"] = (item.get("detail", "") + "\n" + d).strip()
        if e:
            item["note"] = e
        day["items"].append(item)
    return day


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xlsx"); ap.add_argument("--year", type=int, required=True)
    ap.add_argument("--title", default="旅遊行程"); ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    days, ov, skipped = [], [], []
    for name, rows in read_xlsx(a.xlsx):
        if re.match(r"\s*\d{1,2}/\d{1,2}\s*\(Day", name):
            days.append(day_sheet(name, rows, a.year))
        elif any(k in name for k in ("交通", "訂票")) and rows:
            ov.append("【" + name + "】")
            for r in rows[1:]:
                line = " | ".join(x for x in r if x)
                if line:
                    ov.append(line)
        else:
            skipped.append(name)
    days.sort(key=lambda d: d["date"])
    if not days:
        sys.exit("找不到「M/D (Day N)」格式的分頁")
    trip = {"title": a.title, "subtitle": f'{days[0]["date"]} – {days[-1]["date"]}',
            "overview": {"notes": ov}, "days": days}
    open(a.out, "w", encoding="utf-8").write(json.dumps(trip, ensure_ascii=False, indent=2))
    print(f"OK {len(days)} 天 -> {a.out}" + (f"; 略過分頁: {', '.join(skipped)}" if skipped else ""))


if __name__ == "__main__":
    main()
