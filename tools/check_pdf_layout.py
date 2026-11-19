"""Manual visual probe: crop supplied reference screenshots, detect and outline text.

No network requests. Outputs stay in artifacts/070. Requires Pillow and a built
PDF worker with the deployed detection runtime. Inputs: reference PNG paths.
"""
import json
import subprocess
import sys
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
output = root / "artifacts/070"
output.mkdir(parents=True, exist_ok=True)


def worker(request, name):
    input_path, result_path = output / f"{name}-request.json", output / f"{name}.json"
    input_path.write_text(json.dumps(request, ensure_ascii=False), encoding="utf-8")
    subprocess.run([str(root / "build/yiyi_pdf_worker.exe"), str(input_path), str(result_path)],
                   capture_output=True, check=True, timeout=90)
    data = json.loads(result_path.read_text(encoding="utf-8"))
    if "error" in data:
        raise RuntimeError(data["error"])
    return data


for index, path in enumerate(sys.argv[1:], 1):
    image = Image.open(path).convert("RGB")
    # User reference screenshots have application chrome and a paired-page view.
    box = (0, 45, 798, 1005) if index == 1 else (0, 47, 790, 1007)
    image = image.crop(box)
    source = output / f"reference-{index}.pdf"
    image.save(source, resolution=100)
    doc = worker(dict(action="inspect", source=str(source)), f"reference-{index}-inspect")
    result = worker(dict(action="recognitionImage", source=str(source), document=doc, page=0,
                         region=[0, 0, 1, 1], pixels=2400, image=str(output / f"reference-{index}.png"),
                         detectLayout=True), f"reference-{index}-layout")
    preview = Image.open(output / f"reference-{index}.png").convert("RGB")
    draw = ImageDraw.Draw(preview)
    page = doc["pages"][0]
    sx, sy = preview.width / page["width"], preview.height / page["height"]
    for n, block in enumerate(result["blocks"]):
        x, y, w, h = block["rect"]
        draw.rectangle((x * sx, y * sy, (x + w) * sx, (y + h) * sy), outline="red", width=2)
        draw.text((x * sx - 20, y * sy), str(n), fill="red")
    preview.thumbnail((900, 1200))
    preview.save(output / f"reference-{index}-regions.png")
    print(index, "regions", len(result["blocks"]))
