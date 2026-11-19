"""Offline PDF stress fixtures, process measurements and output verification.

Use build/validation-python with Pillow and psutil. The application
and portable release do not depend on Python. Never uses an AI service.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import random
import sqlite3
import subprocess
import time

from PIL import Image, ImageChops, ImageDraw, ImageFont
import psutil

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def jpeg(scan):
    if scan:
        # Every image stream is used by an actual page. The large file is not
        # produced by unused padding, and decoding is part of preview/export.
        image = Image.frombytes("RGB", (1000, 1415), random.Random(413).randbytes(1000 * 1415 * 3))
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 999, 190), fill="white")
        font = ImageFont.truetype(r"C:\Windows\Fonts\arial.ttf", 28)
        draw.text((75, 80), "Synthetic scan heading", fill="black", font=font)
        draw.line((75, 220, 925, 220), fill="black", width=4)
        draw.rectangle((740, 1100, 910, 1260), fill="#217859", outline="white", width=5)
    else:
        image = Image.new("RGB", (160, 100), "#217859")
        draw = ImageDraw.Draw(image)
        draw.ellipse((30, 15, 100, 85), fill="#f4cc54")
    buffer = io.BytesIO()
    image.save(buffer, "JPEG", quality=65 if scan else 90)
    return buffer.getvalue(), image.size


def make_fixture(path, count, kind):
    images = {False: jpeg(False), True: jpeg(True)}
    offsets = [0]
    with path.open("wb") as file:
        def obj(body):
            number = len(offsets)
            offsets.append(file.tell())
            file.write(f"{number} 0 obj\n".encode() + body + b"\nendobj\n")
        file.write(b"%PDF-1.7\n")
        obj(b"<< /Type /Catalog /Pages 2 0 R >>")
        kids = " ".join(f"{4 + 3 * i} 0 R" for i in range(count))
        obj(f"<< /Type /Pages /Count {count} /Kids [{kids}] >>".encode())
        obj(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
        for page in range(count):
            scanned = kind == "scan" or (kind == "mixed" and page % 2 == 1)
            obj((f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 3 0 R >> "
                 f"/XObject << /Im {6 + 3 * page} 0 R >> >> /Contents {5 + 3 * page} 0 R >>").encode())
            if scanned:
                content = b"q 595 0 0 842 0 0 cm /Im Do Q"
            else:
                commands = [f"BT /F1 20 Tf 1 0 0 1 40 780 Tm (Sample page {page + 1}) Tj ET"]
                lines = [
                    "This paragraph explains the structure of a large translated document.",
                    "The title, paragraphs, pictures and rules keep their original positions.",
                    "Progress is saved so the reader can stop work and continue later.",
                ]
                for paragraph in range(6):
                    for row, line in enumerate(lines):
                        commands.append(f"BT /F1 11 Tf 1 0 0 1 40 {710 - paragraph * 90 - row * 15} Tm ({line}) Tj ET")
                commands += ["q 120 0 0 75 40 40 cm /Im Do Q", "0 0 0 RG 1 w 40 160 m 555 160 l S"]
                content = "\n".join(commands).encode()
            obj(f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"\nendstream")
            data, (width, height) = images[scanned]
            obj((f"<< /Type /XObject /Subtype /Image /Width {width} /Height {height} /ColorSpace /DeviceRGB "
                 f"/BitsPerComponent 8 /Filter /DCTDecode /Length {len(data)} >>\nstream\n").encode() + data + b"\nendstream")
        xref = file.tell()
        file.write(f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode())
        for offset in offsets[1:]:
            file.write(f"{offset:010d} 00000 n \n".encode())
        file.write(f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())


def environment():
    env = dict(os.environ)
    env["PATH"] = r"C:\Qt\6.12.0\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;" + env["PATH"]
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_QUICK_BACKEND"] = "software"
    env["QT_QPA_FONTDIR"] = r"C:\Windows\Fonts"
    return env


def measure(source, output):
    output.mkdir()
    log = (output / "console.log").open("wb")
    process = subprocess.Popen([str(ROOT / "build/yiyi_pdf_large_check.exe"), str(source), str(output)],
                               stdout=log, stderr=log, env=environment(), creationflags=subprocess.CREATE_NO_WINDOW)
    parent = psutil.Process(process.pid)
    samples, peaks = [], {}
    started, last_sample, last_message = time.monotonic(), 0, 0
    while process.poll() is None:
        elapsed = time.monotonic() - started
        processes = []
        try:
            processes = [parent, *parent.children(recursive=True)]
        except psutil.Error:
            pass
        sample = {"seconds": round(elapsed, 2)}
        for child in processes:
            try:
                name, memory = child.name(), child.memory_info()
                values = {"rssMiB": memory.rss / 1048576,
                          "privateMiB": getattr(memory, "private", memory.pagefile) / 1048576}
                sample[name] = values
                peak = peaks.setdefault(name, {"rssMiB": 0, "privateMiB": 0})
                for key, value in values.items():
                    peak[key] = max(peak[key], value)
            except psutil.Error:
                pass
        if elapsed - last_sample > 1:
            samples.append(sample)
            last_sample = elapsed
        if elapsed - last_message > 20:
            progress = output / "progress.json"
            if progress.exists():
                value = json.loads(progress.read_text(encoding="utf-8"))
                print(f"{elapsed:.0f}s phase={value['phase']} pages={value.get('analyzed')} {value.get('operation', '')}", flush=True)
            last_message = elapsed
        if elapsed > 1900:
            for child in reversed(processes):
                try:
                    child.kill()
                except psutil.Error:
                    pass
            raise RuntimeError("Stress run timed out")
        time.sleep(.1)
    log.close()
    (output / "memory.json").write_text(json.dumps({"peaks": peaks, "samples": samples}, indent=2), encoding="utf-8")
    if process.returncode:
        raise RuntimeError(f"Controller exercise failed ({process.returncode}); see {output / 'report.json'}")
    return peaks


def worker(output, name, request):
    task, result = output / f"{name}-request.json", output / f"{name}.json"
    task.write_text(json.dumps(request, ensure_ascii=False), encoding="utf-8")
    completed = subprocess.run([str(ROOT / "build/yiyi_pdf_worker.exe"), str(task), str(result)],
                               env=environment(), capture_output=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
    (output / f"{name}.stderr.log").write_bytes(completed.stderr)
    completed.check_returncode()
    value = json.loads(result.read_text(encoding="utf-8"))
    if value.get("error"):
        raise RuntimeError(value["error"])
    return value


def verify(source, output, count, original_hash, kind):
    target = output / "translated.pdf"
    if digest(source) != original_hash:
        raise AssertionError("Source file changed")
    exported = worker(output, "export-info", dict(action="info", source=str(target)))
    assert exported["pageCount"] == count
    for first in range(0, count, 8):
        batch = worker(output, f"verify-{first + 1}", dict(action="inspectPages", source=str(target), document=exported,
                       first=first, count=8, quickSourceCheck=True))
        for page in batch["pages"]:
            index = page["number"]
            assert abs(page["width"] - 595) < .01 and abs(page["height"] - 842) < .01
            expected = "扫描译文" if kind == "scan" or (kind == "mixed" and index % 2 == 1) else "中文译文"
            assert expected in page.get("text", ""), f"Missing searchable Chinese on page {index + 1}"
        if first % 200 == 0:
            print(f"Verify searchable text: {min(first + 8, count)}/{count}", flush=True)
    report = json.loads((output / "report.json").read_text(encoding="utf-8"))
    with sqlite3.connect(report["database"]) as store:
        metadata = json.loads(store.execute("SELECT data FROM metadata WHERE id=1").fetchone()[0])
        for index in sorted({0, min(31, count - 1), min(32, count - 1), count // 2, count - 1}):
            page = json.loads(store.execute("SELECT data FROM pages WHERE number=?", (index,)).fetchone()[0])
            expected = output / f"page-{index + 1}-expected.png"
            actual = output / f"page-{index + 1}-actual.png"
            worker(output, f"page-{index + 1}-expected", dict(action="render", source=str(source), document=metadata, pageData=page,
                   page=index, pixels=800, quickSourceCheck=True, original=str(output / "original.png"), translated=str(expected)))
            batch = worker(output, f"page-{index + 1}-inspect", dict(action="inspectPages", source=str(target), document=exported,
                           first=index, count=1, quickSourceCheck=True))
            worker(output, f"page-{index + 1}-actual", dict(action="render", source=str(target), document=exported, pageData=batch["pages"][0],
                   page=index, pixels=800, quickSourceCheck=True, original=str(actual), translated=str(output / "reopened.png")))
            with Image.open(expected) as a, Image.open(actual) as b:
                difference = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
                assert difference.getbbox() is None, f"Preview/export mismatch on page {index + 1}"
    report["verification"] = {"sourceUnchanged": True, "searchableChinesePages": count, "pageSizesUnchanged": True, "sampledPreviewPixelsEqual": True}
    report["sourceSha256"] = original_hash
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("native", "scan", "mixed"), required=True)
    parser.add_argument("--pages", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.pages < 1:
        parser.error("pages must be positive")
    output = args.output.resolve()
    if (output.joinpath("run").exists()):
        parser.error("use a fresh output directory to preserve previous measurements")
    output.mkdir(parents=True, exist_ok=True)
    source = output / "source.pdf"
    make_fixture(source, args.pages, args.kind)
    original_hash = digest(source)
    print(f"Fixture {args.kind}: {args.pages} pages, {source.stat().st_size / 1048576:.1f} MiB", flush=True)
    peaks = measure(source, output / "run")
    report = verify(source, output / "run", args.pages, original_hash, args.kind)
    report["memoryPeaks"] = peaks
    (output / "verified.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
