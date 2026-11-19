"""Opt-in portable release verification, without using the user's AI settings.

Usage: python tools/check_release.py <zip> <scan-pdf> <saved-layout-result-json>
Checks the actual extracted binaries with external Qt paths removed. The model
result is an existing or synthetic test artifact; this tool never calls an AI endpoint.
"""
import hashlib
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("archive", type=Path)
parser.add_argument("source", type=Path)
parser.add_argument("model_result", type=Path)
parser.add_argument("--build-dir", type=Path, default=root / "build")
args = parser.parse_args()
archive, source, model_result = (p.resolve() for p in (args.archive, args.source, args.model_result))
build = args.build_dir.resolve()
version = re.search(r"project\(Yiyi VERSION ([\d.]+)", (root / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
output = root / "artifacts" / version.replace(".", "")
extraction = output / ("便携包验证-" + uuid.uuid4().hex[:8])
extraction.mkdir(parents=True)
report = {"archive": str(archive), "extraction": str(extraction), "aiCalls": 0}


def sha(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


env = dict(os.environ)
windows = Path(env.get("WINDIR", r"C:\Windows"))
env["PATH"] = os.pathsep.join(str(windows / name) for name in ("System32", ".", "System32/Wbem"))
for name in list(env):
    if name.startswith(("QT_", "QML", "YIYI_")):
        env.pop(name)
env["QT_QUICK_BACKEND"] = "software"


def run(exe, args, name, timeout=90):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    result = subprocess.run([str(exe), *map(str, args)], env=env, cwd=exe.parent,
                            capture_output=True, timeout=timeout, startupinfo=startup,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    (output / f"{name}.stdout.log").write_bytes(result.stdout)
    (output / f"{name}.stderr.log").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f"{name} exited with {result.returncode}; see its local log")


def worker(package, request, name):
    input_path, result_path = output / f"{name}-request.json", output / f"{name}.json"
    input_path.write_text(json.dumps(request, ensure_ascii=False), encoding="utf-8")
    run(package / "yiyi_pdf_worker.exe", [input_path, result_path], name)
    data = json.loads(result_path.read_text(encoding="utf-8"))
    if data.get("error"):
        raise RuntimeError(data["error"])
    return data


try:
    with zipfile.ZipFile(archive) as package_zip:
        files = [item for item in package_zip.infolist() if not item.is_dir()]
        for item in files:
            if not (extraction / item.filename).resolve().is_relative_to(extraction):
                raise RuntimeError("Archive path escapes its extraction directory")
        package_zip.extractall(extraction)
        for item in files:
            if sha(extraction / item.filename) != hashlib.sha256(package_zip.read(item)).hexdigest():
                raise RuntimeError(f"Extracted file differs: {item.filename}")
    report["filesVerified"] = len(files)
    report["archiveSha256"] = sha(archive)
    package = extraction / "Yiyi"
    for relative in ("Yiyi.exe", "yiyi_pdf_worker.exe", "yiyi_ocr_worker.exe", "ocr/YiyiOcr.dll", "pdf/YiyiPdfSans-Regular.otf", "pdf/YiyiPdfSerif-Regular.otf", "pdf/qpdf/qpdf.dll"):
        if sha(package / relative) != sha(build / relative):
            raise RuntimeError(f"Release differs from the build: {relative}")
    for component in ("ocr", "pdf"):
        for entry in json.loads((package / component / "checksums.json").read_text(encoding="utf-8-sig")):
            if sha(package / component / entry["file"]) != entry["sha256"].lower():
                raise RuntimeError(f"Runtime manifest mismatch: {component}/{entry['file']}")
    report["buildAndRuntimeHashesMatch"] = True

    original_source = source
    original_hash = sha(original_source)
    source = extraction / "中文 PDF 原文.pdf"
    shutil.copyfile(original_source, source)
    document = worker(package, dict(action="inspect", source=str(source)), "portable-inspect")
    result = json.loads(model_result.read_text(encoding="utf-8"))
    if result.get("error") or not result.get("blocks"):
        raise RuntimeError("Supply a saved recognition/translation layout result")
    report["translationFixture"] = result.get("fixtureKind", "saved layout result")
    scan = {"blocks": result["blocks"], "source": result["source"], "translation": result["translation"], "layout": result.get("layout", {})}
    document["pages"][0]["scan"] = scan
    profile = extraction / "isolated-profile"
    (profile / "pdf-tasks").mkdir(parents=True)
    cache = {"version": 4, "hash": document["hash"], "scans": [{"page": 0, "scan": scan}]}
    (profile / "pdf-tasks" / (document["hash"] + ".json")).write_text(json.dumps(cache, ensure_ascii=False), encoding="utf-8")
    for label, folder in (("portable-start", package), ("build-start", build)):
        run(folder / "Yiyi.exe", ["--data-dir", profile, "--no-desktop", "--pdf", source,
                                  "--smoke-test", "--screenshot", output / f"{label}.png"], label, 30)
        if not (output / f"{label}.png").is_file():
            raise RuntimeError(f"Startup screenshot missing: {label}")
        report[label] = "passed"

    image_path = output / "portable-recognition.png"
    detection = worker(package, dict(action="recognitionImage", source=str(source), document=document, page=0,
                                      region=[0, 0, 1, 1], pixels=1800, image=str(image_path), detectLayout=True), "portable-detection")
    if not detection.get("blocks"):
        raise RuntimeError("Packaged detector returned no regions")
    report["detectedRegions"] = len(detection["blocks"])
    ocr_path = output / "portable-ocr.json"
    run(package / "yiyi_ocr_worker.exe", [image_path, ocr_path], "portable-ocr")
    ocr = json.loads(ocr_path.read_text(encoding="utf-8"))
    if not ocr.get("text", "").strip():
        raise RuntimeError("Packaged OCR returned no text")
    report["ocrCharacters"] = len(ocr["text"])

    translated = output / "便携包中文译文.pdf"
    database = profile / "pdf-tasks" / (document["hash"] + ".sqlite")
    if not database.is_file():
        raise RuntimeError("Portable application did not migrate the PDF task")
    worker(package, dict(action="export", source=str(source), taskDatabase=str(database), destination=str(translated)), "portable-export")
    report["sqliteMigrationAndWorkerRead"] = True
    reopened = worker(package, dict(action="inspect", source=str(translated)), "portable-reopen")
    if len(reopened["pages"]) != len(document["pages"]):
        raise RuntimeError("Export changed the page count")
    marker = next(b["translation"] for b in result["blocks"] if b.get("translation") and not b.get("fixed") and not b.get("keep"))
    text = reopened["pages"][0].get("text", "")
    if re.sub(r"\s", "", marker) not in re.sub(r"\s", "", text):
        raise RuntimeError("Exported translation is not searchable")
    report["exportPages"] = len(reopened["pages"])
    report["searchableTranslation"] = True
    report["unicodePdfPaths"] = True
    report["translatedPdf"] = str(translated)
    report["sourceUnchanged"] = sha(source) == original_hash and sha(original_source) == original_hash
    if not report["sourceUnchanged"]:
        raise RuntimeError("Source PDF changed")
    # Exercise native and raster multi-column/table exports from the actual
    # extracted worker, in addition to the scan fixture above.
    complex_results = {}
    for label in ("native-two", "native-three", "scan-two", "scan-three"):
        fixture = output / "complex-layouts" / label
        if not (fixture / "layout.json").is_file():
            raise RuntimeError(f"Missing {label} fixture: run yiyi_pdf_tests structuredPage with YIYI_PDF_ARTIFACT_DIR={output / 'complex-layouts'}")
        source_pdf = fixture / "source.pdf"
        source_digest = sha(source_pdf)
        manifest = json.loads((fixture / "layout.json").read_text(encoding="utf-8"))
        original = worker(package, dict(action="inspect", source=str(source_pdf)), label + "-inspect")
        if original["hash"] != manifest["hash"]:
            raise RuntimeError(f"Stale complex fixture: {label}")
        target = output / (label + ".pdf")
        worker(package, dict(action="export", source=str(source_pdf), document=manifest, destination=str(target)), label + "-export")
        exported = worker(package, dict(action="inspect", source=str(target)), label + "-reopen")
        if len(exported["pages"]) != len(original["pages"]):
            raise RuntimeError(f"Page count changed: {label}")
        expected = output / (label + "-preview.png")
        actual = output / (label + "-exported.png")
        worker(package, dict(action="render", source=str(source_pdf), document=manifest, page=0,
                             original=str(output / (label + "-original.png")), translated=str(expected)), label + "-preview")
        worker(package, dict(action="render", source=str(target), document=exported, page=0,
                             original=str(actual), translated=str(output / (label + "-reopened.png"))), label + "-render")
        if sha(expected) != sha(actual) or sha(source_pdf) != source_digest:
            raise RuntimeError(f"Preview/export or source integrity failed: {label}")
        complex_results[label] = "preview/export identical; source unchanged"
    report["complexLayouts"] = complex_results
    report["error"] = ""
except Exception as error:
    report["error"] = str(error)
finally:
    (output / "package-check.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=True))
sys.exit(1 if report.get("error") else 0)
