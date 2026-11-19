"""Audit and merge the explicitly assigned ranges of a real book run.

No network or credentials are used. Each page is selected once from its owning
range; completed counts in unrelated copies of the database are never summed.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import unicodedata


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, data):
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


def connection(path):
    return sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True)


def block_issues(block):
    if block.get("keep"):
        return []
    issues = []
    text = block.get("source", "") + " " + block.get("translation", "")
    if re.search(r"[\[［]\s*(unclear|unreadable|illegible|不清楚|不清晰|无法辨认|模糊)\s*[\]］]", text, re.I):
        issues.append("unclear")
    if block.get("stale"):
        issues.append("stale")
    if block.get("layoutError"):
        issues.append("layout")
    if block.get("backgroundReview") and not block.get("backgroundConfirmed"):
        issues.append("background")
    return issues


def summarize(number, page):
    summary = {"number": number}
    for key in ("width", "height", "warning", "notice", "kind", "baseLayoutHash", "analyzed", "analysisError", "extractionVersion"):
        if key in page:
            summary[key] = page[key]
    scan = page.get("kind") == "scan" or "scan" in page
    data = page.get("scan", {}) if scan else page
    total = completed = preserved = review = 0
    reasons = Counter()
    for block in data.get("blocks", []):
        issues = block_issues(block)
        review += bool(issues)
        reasons.update(issues)
        preserved += bool(block.get("keep") or block.get("backgroundMode") == "original")
        if block.get("fixed") or block.get("keep"):
            continue
        total += 1
        completed += bool(block.get("translation", "").strip()) and not block.get("stale")
    summary.update(scan=scan, recognized=scan and bool(data.get("source", "").strip()),
                   total=total, completed=completed, preserved=preserved, hasBlocks=bool(data.get("blocks")),
                   issueVersion=1, review=review, issues=dict(reasons))
    return summary


def collect(manifest):
    pages = {}
    metadata = None
    identity = None
    sources = []
    for part in manifest["ranges"]:
        with connection(part["database"]) as db:
            db.execute("BEGIN")
            meta = json.loads(db.execute("SELECT data FROM metadata WHERE id=1").fetchone()[0])
            assert meta["hash"] == manifest["sourceSha256"], "Source hash mismatch"
            assert meta["pageCount"] == manifest["pageCount"], "Page count mismatch"
            snapshot = meta["snapshot"]
            provider = snapshot["provider"]
            assert provider["model"] == manifest["model"], "Model mismatch"
            assert provider.get("reasoningEffort") == manifest["reasoningEffort"], "Reasoning setting mismatch"
            if identity is not None:
                assert snapshot == identity, "Translation settings differ between ranges"
            else:
                identity = snapshot
                metadata = meta
            rows = db.execute("SELECT number,data FROM pages WHERE number>=? AND number<? ORDER BY number",
                              (part["first"] - 1, part["last"])).fetchall()
            assert len(rows) == part["last"] - part["first"] + 1, "Range has missing pages"
            for number, raw in rows:
                assert number not in pages, "Overlapping page assignments"
                pages[number] = json.loads(raw)
            sources.append({"first": part["first"], "last": part["last"], "database": part["database"]})
    assert sorted(pages) == list(range(manifest["pageCount"])), "Page assignments do not cover the book"
    return metadata, pages, sources


def audit(pages):
    totals = Counter()
    versions = Counter()
    pending = []
    issues = []
    preserved = []
    unchanged = []
    for number, page in sorted(pages.items()):
        summary = summarize(number, page)
        versions[page.get("extractionVersion")] += 1
        totals.update({key: summary[key] for key in ("total", "completed", "preserved", "review")})
        if (not page.get("analyzed") or page.get("analysisError") or summary["total"] != summary["completed"]
                or (summary["scan"] and not summary["hasBlocks"]) or (not summary["scan"] and page.get("warning"))):
            pending.append({"page": number + 1, "remaining": summary["total"] - summary["completed"],
                            "warning": page.get("analysisError", page.get("warning", "")), "scan": summary["scan"]})
        for b in page.get("scan", page).get("blocks", []):
            item = {"page": number + 1, "id": b["id"], "source": b.get("source", ""),
                    "translation": b.get("translation", ""), "code": b.get("code", False), "fixed": b.get("fixed", False)}
            if block_issues(b):
                issues.append({**item, "issues": block_issues(b), "layoutError": b.get("layoutError", "")})
            if b.get("keep"):
                preserved.append({**item, "reason": b.get("preserveReason", b.get("reviewNote", ""))})
            elif b.get("translation") and not b.get("fixed") and not re.search(r"[\u3400-\u9fff]", b["translation"]):
                unchanged.append(item)
    return {"pageCount": len(pages), "totals": dict(totals), "versions": dict(versions),
            "pendingPages": pending, "issues": issues, "preservedBlocks": preserved, "nonChineseTranslations": unchanged}


def merge(manifest, destination, metadata, pages, report):
    assert not report["pendingPages"], "Unfinished pages prevent merging"
    assert not report["issues"], "Review issues must be resolved before merging"
    assert len(report["versions"]) == 1, "Extraction versions differ; revisit the ranges"
    with Path(manifest["source"]).open("rb") as source:
        assert hashlib.file_digest(source, "sha256").hexdigest() == manifest["sourceSha256"], "Source PDF changed"
    destination = Path(destination)
    assert not destination.exists(), "Choose a new destination; an existing task is never overwritten"
    destination.parent.mkdir(parents=True, exist_ok=True)
    metadata = dict(metadata, workflow={}, extractionVersion=next(iter(report["versions"])))
    with sqlite3.connect(destination) as db:
        db.execute("CREATE TABLE metadata (id INTEGER PRIMARY KEY CHECK(id=1), data BLOB NOT NULL)")
        db.execute("CREATE TABLE pages (number INTEGER PRIMARY KEY CHECK(number>=0), data BLOB NOT NULL, summary BLOB NOT NULL)")
        encoded = lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        db.execute("INSERT INTO metadata VALUES(1,?)", (encoded(metadata),))
        for number, page in sorted(pages.items()):
            db.execute("INSERT INTO pages VALUES(?,?,?)", (number, encoded(page), encoded(summarize(number, page))))
        assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    with connection(destination) as db:
        assert db.execute("SELECT count(*) FROM pages").fetchone()[0] == manifest["pageCount"]


def verify(manifest, pages, pdf):
    import pymupdf
    problems = []
    counts = Counter()
    plain = lambda value: re.sub(r"\s+", "", unicodedata.normalize("NFKC", re.sub(r"</?(?:b|i|sup|sub)>", "", value)))
    with pymupdf.open(pdf) as output, pymupdf.open(manifest["source"]) as source:
        assert len(output) == len(source) == manifest["pageCount"], "Export page count mismatch"
        for number, page in sorted(pages.items()):
            actual = output[number]
            if any(abs(a - b) > .1 for a, b in zip(actual.rect, source[number].rect)):
                problems.append({"page": number + 1, "problem": "size"})
            text = plain(actual.get_text())
            counts["chineseCharacters"] += len(re.findall(r"[\u3400-\u9fff]", text))
            if "\ufffd" in text:
                problems.append({"page": number + 1, "problem": "replacement character"})
            if re.search(r"\[(?:unclear|不清楚|不清晰)\]", text, re.I):
                problems.append({"page": number + 1, "problem": "unresolved recognition"})
            for block in page.get("scan", page).get("blocks", []):
                if block.get("fixed") or block.get("keep") or not block.get("translation"):
                    continue
                counts["translatedBlocks"] += 1
                expected = plain(block["translation"])
                sequences = re.findall(r"[\u3400-\u9fff]{2,}", expected)
                missing = [part for part in sequences if part not in text]
                if missing:
                    problems.append({"page": number + 1, "id": block["id"], "problem": "missing translated text", "missing": missing})
            counts["pagesChecked"] += 1
    return {"pdf": str(pdf), "counts": dict(counts), "problems": problems, "success": not problems}


def drawing_geometry(drawing):
    """Normalize equivalent rectangle operators and explicit closed paths.

    PDFium serializes compound `re` paths as four lines; MuPDF may report
    those as a quad. Its normalized rectangles lose the original width sign,
    so compare edge geometry rather than inferred rectangle winding. Fill
    rules, colors and opacity are checked separately; visual review remains
    necessary for clipping and compound fills.
    """
    segments = []
    start = end = None
    def coordinates(value):
        if isinstance(value, (str, int, float, type(None))):
            return value
        return tuple(coordinates(v) for v in value)
    for item in drawing['items']:
        kind = item[0]
        if kind == 're':
            rect, winding = item[1:]
            points = [rect.tl, rect.tr, rect.br, rect.bl] if winding == -1 else [rect.bl, rect.br, rect.tr, rect.tl]
            segments.extend(('l', tuple(a), tuple(b)) for a, b in zip(points, points[1:] + points[:1]))
            start = end = tuple(points[0])
        elif kind == 'qu':
            quad = item[1]
            points = [quad.ul, quad.ll, quad.lr, quad.ur]
            segments.extend(('l', tuple(a), tuple(b)) for a, b in zip(points, points[1:] + points[:1]))
            start = end = tuple(points[0])
        elif kind in ('l', 'c'):
            points = [tuple(p) for p in item[1:]]
            if end != points[0]:
                start = points[0]
            end = points[-1]
            segments.append((kind, *points))
        else:
            segments.append(coordinates(item))
            start = end = None
    if drawing.get('closePath') and start is not None and start != end:
        segments.append(('l', end, start))
    return sorted(('l', *sorted(item[1:])) if item[0] == 'l' else item for item in segments)


def drawings_equivalent(before, after):
    def close(a, b, tolerance):
        if isinstance(a, (float, int)) and isinstance(b, (float, int)):
            return abs(a - b) <= tolerance
        if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
            return len(a) == len(b) and all(close(x, y, tolerance) for x, y in zip(a, b))
        return a == b

    if before['type'] != after['type']:
        return False
    if before.get('even_odd') != after.get('even_odd'):
        return False
    # The independent engines' calibrated/CMYK conversions differ by up to
    # two 8-bit channel steps (observed maximum 0.00538), including quantization.
    for key in ('fill', 'color'):
        if not close(before.get(key), after.get(key), 2 / 255 + 1e-5):
            return False
    for key in ('fill_opacity', 'stroke_opacity'):
        if not close(before.get(key), after.get(key), 1 / 255 + 1e-5):
            return False
    if not close(before.get('width'), after.get('width'), .01):
        return False
    dash = lambda d: re.findall(r'[-+]?(?:\d*\.)?\d+', d.get('dashes') or '')
    if not close([float(v) for v in dash(before)], [float(v) for v in dash(after)], .01):
        return False
    # Two hundredths of a PDF point covers decimal serialization round-off
    # while still rejecting a displaced or missing line at normal zoom.
    a, b = drawing_geometry(before), drawing_geometry(after)
    if len(a) != len(b):
        return False
    if close(a, b, .02):
        return True
    # Tiny coordinate differences can change the sort order of adjacent edges.
    for segment in a:
        match = next((i for i, candidate in enumerate(b) if close(segment, candidate, .02)), None)
        if match is None:
            return False
        b.pop(match)
    return True


def verify_artwork(manifest, pages, pdf):
    """Independently compare retained native images and vector paths.

    Scanned/rasterized pages are listed separately: their background is rebuilt
    during text replacement and needs pixel/visual review instead of a claim
    that the original PDF objects were retained.
    """
    import pymupdf
    counts = Counter()
    problems = []
    raster_pages = []

    with pymupdf.open(manifest["source"]) as source, pymupdf.open(pdf) as output:
        assert len(source) == len(output) == manifest["pageCount"]
        for number, page in sorted(pages.items()):
            blocks = page.get("scan", page).get("blocks", [])
            active = any(b.get("translation") and not b.get("keep") and not b.get("fixed")
                         and b.get("backgroundMode") != "original" for b in blocks)
            raster = "scan" in page or page.get("kind") == "scan" or page.get("rasterBackground")
            if raster and active:
                raster_pages.append(number + 1)
                continue
            before, after = source[number], output[number]
            images = after.get_image_info(hashes=True)
            for original in before.get_image_info(hashes=True):
                match = next((i for i, candidate in enumerate(images)
                              if original["digest"] == candidate["digest"]
                              and all(abs(a - b) <= .05 for a, b in zip(original["bbox"], candidate["bbox"]))), None)
                if match is None:
                    problems.append({"page": number + 1, "problem": "image pixels or position changed", "bbox": original["bbox"]})
                else:
                    images.pop(match)
                    counts["unchangedImageInstances"] += 1
            original_paths = before.get_drawings()
            exported_paths = after.get_drawings()
            missing = 0
            for original in original_paths:
                match = next((i for i, candidate in enumerate(exported_paths) if drawings_equivalent(original, candidate)), None)
                if match is None:
                    missing += 1
                else:
                    exported_paths.pop(match)
            if missing:
                problems.append({"page": number + 1, "problem": "native vector paths changed", "paths": missing})
            counts["unchangedVectorPaths"] += len(original_paths) - missing
            counts["nativeOrPreservedPagesChecked"] += 1
    return {"counts": dict(counts), "rasterPagesForVisualReview": raster_pages, "problems": problems, "success": not problems}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--merge", type=Path)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    manifest = read_json(args.manifest)
    metadata, pages, sources = collect(manifest)
    report = audit(pages)
    report["sources"] = sources
    write_json(args.report, report)
    print(json.dumps({key: report[key] for key in ("pageCount", "totals", "versions")}))
    print(f"Pending pages: {len(report['pendingPages'])}; review blocks: {len(report['issues'])}; preserved blocks: {len(report['preservedBlocks'])}")
    if args.merge:
        merge(manifest, args.merge, metadata, pages, report)
        print("Merged", args.merge)
    if args.verify:
        result = verify(manifest, pages, args.verify)
        result["artwork"] = verify_artwork(manifest, pages, args.verify)
        result["success"] = result["success"] and result["artwork"]["success"]
        write_json(args.report.with_name(args.report.stem + "-export.json"), result)
        print(json.dumps({"success": result["success"], "counts": result["counts"], "problems": len(result["problems"])}))
        if not result["success"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
