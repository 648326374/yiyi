import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from zipfile import ZipFile


def run(executable, fixture_root):
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        word = directory / "原稿 #100%.DOCX"
        ppt = directory / "原稿 #100%.PPTX"
        shutil.copyfile(fixture_root / "sample.docx", word)
        shutil.copyfile(fixture_root / "sample.pptx", ppt)
        for file in ("fields.docx", "images-only.docx"):
            shutil.copyfile(fixture_root / file, directory / file)
        shutil.copyfile(word, directory / "spoof.pptx")
        (directory / "broken.docx").write_bytes(b"invalid file")
        for test, added_part in (("VBA", "word/VBAProject.bin"), ("unsafe-path", "../foo.xml"), ("signed", "_xmlsignatures/sig1.xml")):
            modified = directory / f"{test}.docx"
            shutil.copyfile(word, modified)
            with ZipFile(modified, "a") as package:
                package.writestr(added_part, "synthetic rejection fixture")

        with ZipFile(word) as original, ZipFile(directory / "DTD.docx", "w") as modified:
            for part in original.namelist():
                payload = original.read(part)
                if part == "_rels/.rels":
                    offset = payload.index(b"?>") + 2 if payload.startswith(b"<?xml") else 0
                    payload = payload[:offset] + b'<!DOCTYPE Relationships [<!ENTITY blocked "DTD">]>' + payload[offset:]
                modified.writestr(part, payload)
        subprocess.run([executable, str(directory)], check=True, timeout=25)

        for kind, source, protected in (
            ("docx", word, ["word/styles.xml", "word/media/image1.png", "word/_rels/document.xml.rels"]),
            ("pptx", ppt, ["ppt/slideMasters/slideMaster1.xml", "ppt/media/image1.png", "ppt/slides/_rels/slide1.xml.rels",
                            "ppt/embeddings/Microsoft_Excel_Sheet1.xlsx"]),
        ):
            assert source.read_bytes() == (fixture_root / f"sample.{kind}").read_bytes()
            with ZipFile(source) as original, ZipFile(directory / f"译文 #100%.{kind}") as translated:
                assert set(original.namelist()) == set(translated.namelist())
                assert all(original.read(part) == translated.read(part) for part in protected)
                if kind == "docx":
                    body = translated.read("word/document.xml")
                    assert b"xmlns:w14=" in body and b"xmlns:wp14=" in body
                else:
                    assert b"Alpha" in translated.read("ppt/charts/chart1.xml")
        with ZipFile(directory / "fit.pptx") as fitted:
            root = ET.fromstring(fitted.read("ppt/slides/slide1.xml"))
            first = root.find(".//{http://schemas.openxmlformats.org/drawingml/2006/main}rPr")
            assert 1400 <= int(first.attrib["sz"]) < 3000


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]))
