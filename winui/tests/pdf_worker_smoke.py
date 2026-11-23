import contextlib
import hashlib
import json
import os
import pathlib
import sqlite3
import struct
import subprocess
import sys
import tempfile


def make_pdf(path):
    first = b"1 0 0 rg 0 0 200 150 re f BT /F1 12 Tf 12 130 Td (First page) Tj ET"
    second = b"0 0 1 rg 0 0 300 200 re f BT /F1 16 Tf 18 180 Td (Second page) Tj ET"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 150] /Resources << /Font << /F1 7 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
        b"<< /Length %d >>\nstream\n%s\nendstream" % (len(first), first),
        b"<< /Length %d >>\nstream\n%s\nendstream" % (len(second), second),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    data = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for index, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data += b"%d 0 obj\n%s\nendobj\n" % (index, obj)
    xref = len(data)
    data += b"xref\n0 %d\n0000000000 65535 f \n" % len(offsets)
    for offset in offsets[1:]:
        data += b"%010d 00000 n \n" % offset
    data += b"trailer << /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(offsets), xref)
    path.write_bytes(data)
    return bytes(data)


def image_size(file):
    data = file.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    return struct.unpack(">II", data[16:24])


def main(executable, output_root, task_smoke):
    output_root = pathlib.Path(output_root).resolve()
    with tempfile.TemporaryDirectory(dir=output_root, prefix="pdf-smoke-") as temp:
        root = pathlib.Path(temp).resolve()
        assert root.parent == output_root
        source = root / "原始 文件.pdf"
        original = make_pdf(source)
        request_file = root / "request.json"
        response_file = root / "response.json"
        system = pathlib.Path(os.environ["SystemRoot"])

        def run(action, **arguments):
            request_file.write_text(json.dumps({"action": action, "source": str(source), **arguments}), encoding="utf-8")
            process = subprocess.run([executable, request_file, response_file], capture_output=True, check=True,
                                     env={**os.environ, "PATH": f"{system / 'System32'};{system}"}, timeout=15)
            reply = json.loads(response_file.read_text(encoding="utf-8"))
            progress = [json.loads(line)["progress"] for line in process.stdout.splitlines()]
            return reply, progress

        info, progress = run("info", progress=True)
        assert info["pageCount"] == 2 and info["version"] == 1
        assert info["hash"] == hashlib.sha256(original).hexdigest()
        assert info["stamp"]["size"] == len(original)
        assert info["stamp"]["path"].endswith("/原始 文件.pdf")
        assert progress == [{"stage": "fingerprint", "completed": len(original), "total": len(original)}]

        database = root / "原有 PDF 任务.sqlite"
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.executescript("CREATE TABLE metadata (id INTEGER PRIMARY KEY CHECK(id=1), data BLOB NOT NULL);"
                             "CREATE TABLE pages (number INTEGER PRIMARY KEY CHECK(number>=0), data BLOB NOT NULL,"
                             " summary BLOB NOT NULL);")
            db.execute("INSERT INTO metadata VALUES(1, ?)", (json.dumps({**info, "storageVersion": 6}).encode("utf-8"),))
            page = {"number": 1, "width": 300, "height": 200, "kind": "text", "blocks": [{"id": "p1-b0", "source": "Keep",
                                                                                       "translation": "保留", "keep": True}]}
            db.execute("INSERT INTO pages VALUES(1, ?, ?)", (json.dumps(page).encode("utf-8"),
                        b'{"number":1,"completed":0,"preserved":1}'))
            db.commit()
        subprocess.run([task_smoke, database], check=True, timeout=15)
        with contextlib.closing(sqlite3.connect(database)) as db:
            data, summary = db.execute("SELECT data, summary FROM pages WHERE number=0").fetchone()
            assert isinstance(data, bytes) and json.loads(data)["kind"] == "scan"
            assert isinstance(summary, bytes) and json.loads(summary)["review"] == 1

        preview = root / "preview.png"
        reply, progress = run("renderOriginal", taskDatabase=str(database), quickSourceCheck=True,
                              progress=True, original=str(preview), page=0, pixels=600)
        assert reply == {"width": 200, "height": 150, "saved": True}
        assert progress == []
        assert image_size(preview) == (600, 450)

        extraction, _ = run("extractPageInput", taskDatabase=str(database), quickSourceCheck=True, page=0)
        assert extraction["text"] == "First page"
        assert extraction["mediaBox"] == [0, 0, 200, 150] and extraction["rotation"] == 0
        assert "".join(item["text"] for item in extraction["glyphs"]) == "Firstpage"
        assert extraction["glyphs"][5]["spaceBefore"]
        for glyph in extraction["glyphs"]:
            assert extraction["objects"][glyph["object"]]["type"] == 1
            assert 0 < glyph["baseline"] < extraction["height"] and glyph["size"] == 12
            assert glyph["fontName"] == "Helvetica"

        second = root / "different.png"
        reply, _ = run("renderOriginal", document=info, original=str(second), page=1, pixels=600)
        assert reply == {"width": 300, "height": 200, "saved": True}
        assert image_size(second) == (600, 400)
        assert second.read_bytes() != preview.read_bytes()

        crop = root / "识别.png"
        reply, _ = run("recognitionImage", document=info, quickSourceCheck=True, page=1,
                       image=str(crop), region=[0.25, 0, 0.5, 0.25], pixels=1500)
        assert reply["saved"] and image_size(crop) == (1500, 500)
        reply, _ = run("recognitionImage", document=info, quickSourceCheck=True, page=1,
                       image=str(crop), region=[0.9, 0, 0.5, 0.25])
        assert "error" in reply

        alias = root / "source alias.pdf"
        os.link(source, alias)
        reply, _ = run("renderOriginal", document=info, quickSourceCheck=True, original=str(alias), page=0)
        assert "error" in reply and source.read_bytes() == original and alias.read_bytes() == original

        reply, _ = run("renderOriginal", document=info, quickSourceCheck=True, original=str(preview), page=2)
        assert "error" in reply
        previous = preview.read_bytes()
        source.write_bytes(original + b"\n")
        reply, _ = run("renderOriginal", document=info, quickSourceCheck=True, original=str(preview), page=0)
        assert "error" in reply and preview.read_bytes() == previous
        source.write_bytes(b"invalid PDF data")
        reply, _ = run("info")
        assert "error" in reply


if __name__ == "__main__":
    main(*sys.argv[1:])
