"""Offline fixtures for the Qt-free qpdf merge API."""

import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def stream(contents, attributes=b""):
    return b"<< /Length %d %s >>\nstream\n%s\nendstream" % (len(contents), attributes, contents)


def create_pdf(path, objects, info=None):
    document = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, content in enumerate(objects, 1):
        offsets.append(len(document))
        document += b"%d 0 obj\n%s\nendobj\n" % (number, content)
    xref = len(document)
    document += b"xref\n0 %d\n0000000000 65535 f \n" % len(offsets)
    for offset in offsets[1:]:
        document += b"%010d 00000 n \n" % offset
    info_ref = b" /Info %d 0 R" % info if info else b""
    document += b"trailer\n<< /Root 1 0 R /Size %d%s >>\nstartxref\n%d\n%%%%EOF\n" % (len(offsets), info_ref, xref)
    path.write_bytes(document)


def create_source(path):
    create_pdf(path, [
        b"<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R /PageMode /UseOutlines >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 300 300] "
        b"/Resources << /XObject << /Fm0 7 0 R >> >> >>",
        b"<< /Type /Page /Parent 2 0 R /Contents [5 0 R 6 0 R] /Annots [9 0 R] >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 8 0 R /Resources <<>> >>",
        stream(b"q\n10 10 m\n"),
        stream(b"20 20 l S\nQ\n"),
        stream(b"q Q\n", b"/Type /XObject /Subtype /Form /BBox [0 0 10 10] "
                           b"/Resources << /Pattern << /P0 13 0 R >> >>"),
        stream(b"30 30 m 40 40 l S\n"),
        b"<< /Type /Annot /Subtype /Link /Rect [10 10 40 40] /Dest [4 0 R /Fit] /P 3 0 R >>",
        b"<< /Type /Outlines /First 11 0 R /Last 11 0 R /Count 1 >>",
        b"<< /Title (Original bookmark) /Parent 10 0 R /Dest [3 0 R /Fit] >>",
        b"<< /Title (Source title) >>",
        stream(b"0 0 10 10 re f\n", b"/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 "
                                    b"/BBox [0 0 10 10] /XStep 10 /YStep 10 /Resources <<>>"),
    ], info=12)


def create_part(path, line):
    create_pdf(path, [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 300 300] /Resources <<>> >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>",
        stream(line),
    ])


def inspect(qpdf, path, environment):
    subprocess.run([qpdf, "--check", path], check=True, capture_output=True, env=environment)
    details = subprocess.check_output([qpdf, "--json", "--json-stream-data=inline", path], env=environment)
    return json.loads(details)


def object_record(pdf, reference):
    return pdf["qpdf"][1]["obj:" + reference]


def contents(pdf, page_index):
    return [base64.b64decode(object_record(pdf, reference)["stream"]["data"])
            for reference in pdf["pages"][page_index]["contents"]]


def main():
    executable, runtime, qpdf, work = (Path(value).resolve() for value in sys.argv[1:])
    work.mkdir(parents=True, exist_ok=True)
    system = Path(os.environ["SystemRoot"])
    environment = {**os.environ, "PATH": os.pathsep.join((str(system / "System32"), str(system)))}
    with tempfile.TemporaryDirectory(prefix="qpdf-merge-", dir=work) as directory:
        folder = Path(directory)
        source, part_one, part_two, merged, copied = (folder / name for name in
            ("原文 source.pdf", "译文-1.pdf", "译文-2.pdf", "合并 merged.pdf", "工作副本.pdf"))
        create_source(source)
        original = source.read_bytes()
        lines = [b"55 55 m 65 65 l S\n", b"75 75 m 85 85 l S\n"]
        create_part(part_one, lines[0])
        create_part(part_two, lines[1])
        subprocess.run([executable, runtime, source, part_one, part_two, merged, copied], check=True, env=environment)
        assert source.read_bytes() == original

        work_copy = inspect(qpdf, copied, environment)
        assert len(work_copy["pages"]) == 1
        coalesced = contents(work_copy, 0)
        assert len(coalesced) == 1
        assert coalesced[0].split() == b"q 10 10 m 20 20 l S Q".split()

        result = inspect(qpdf, merged, environment)
        assert len(result["pages"]) == 2
        assert contents(result, 0) == [lines[0]]
        assert contents(result, 1) == [lines[1]]
        page_one, page_two = (page["object"] for page in result["pages"])
        annotations = object_record(result, page_one)["value"]["/Annots"]
        link = object_record(result, annotations[0])["value"]
        assert link["/Subtype"] == "/Link"
        assert link["/P"] == page_one and link["/Dest"] == [page_two, "/Fit"]
        assert result["outlines"][0]["title"] == "Original bookmark"
        assert result["outlines"][0]["dest"] == [page_one, "/Fit"]
        trailer = result["qpdf"][1]["trailer"]["value"]
        assert object_record(result, trailer["/Info"])["value"]["/Title"] == "u:Source title"
    print("Original annotations, internal links, outlines, and metadata preserved")


if __name__ == "__main__":
    main()
