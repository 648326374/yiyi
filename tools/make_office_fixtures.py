"""Rebuild synthetic Office test documents (development only: python-docx, python-pptx, Pillow)."""
from pathlib import Path
from io import BytesIO
import zipfile
from docx import Document
from docx.shared import Inches
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from pptx import Presentation
from pptx.util import Inches as PptInches, Pt
from pptx.chart.data import CategoryChartData
from pptx.enum.chart import XL_CHART_TYPE
from lxml import etree
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / "tests/fixtures/office"
root.mkdir(parents=True, exist_ok=True)
image = Image.new("RGB", (240, 100), "#e7eef8")
ImageDraw.Draw(image).text((15, 40), "Image text stays original", fill="#243d60")
picture = BytesIO()
image.save(picture, format="PNG")
image_bytes = picture.getvalue()

doc = Document()
doc.add_heading("A practical guide", 0)
p = doc.add_paragraph()
p.add_run("Keep the ")
p.add_run("important meaning").bold = True
p.add_run(" when translating.")
doc.add_paragraph("First item", "List Bullet")
table = doc.add_table(rows=3, cols=2)
table.style = "Table Grid"
table.cell(0, 0).merge(table.cell(0, 1)).text = "Merged header"
for row, values in zip(table.rows[1:], [("Name", "Description"), ("Yiyi", "A local translation tool")]):
    for cell, value in zip(row.cells, values):
        cell.text = value
p = doc.add_paragraph()
p.add_run("Before tab")
p.add_run().add_tab()
p.add_run("After tab")
p.add_run().add_break()
p.add_run("After break")
doc.sections[0].header.paragraphs[0].text = "Internal report"
p = doc.sections[0].footer.paragraphs[0]
field = OxmlElement("w:fldSimple")
field.set(qn("w:instr"), "PAGE")
run = OxmlElement("w:r")
text = OxmlElement("w:t")
text.text = "1"
run.append(text)
field.append(run)
p._p.append(field)
doc.add_picture(BytesIO(image_bytes), width=Inches(2))
doc.add_paragraph("Final paragraph with numbers 2026 and punctuation.")
doc.save(root / "sample.docx")

# Add genuine footnote/endnote relationships without depending on Word.
with zipfile.ZipFile(root / "sample.docx") as bundle:
    package = {name: bundle.read(name) for name in bundle.namelist()}
types = etree.fromstring(package["[Content_Types].xml"])
relations = etree.fromstring(package["word/_rels/document.xml.rels"])
document = etree.fromstring(package["word/document.xml"])
body = document.find(qn("w:body"))
for tag in ["footnote", "endnote"]:
    p = OxmlElement("w:p")
    run = OxmlElement("w:r")
    text = OxmlElement("w:t")
    text.text = "Read the " + tag
    run.append(text); p.append(run)
    run = OxmlElement("w:r")
    reference = OxmlElement("w:" + tag + "Reference")
    reference.set(qn("w:id"), "1"); run.append(reference); p.append(run)
    body.insert(len(body) - 1, p)
    part = "word/" + tag + "s.xml"
    namespace = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
    package[part] = (f'<w:{tag}s {namespace}>'
        f'<w:{tag} w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:{tag}>'
        f'<w:{tag} w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:{tag}>'
        f'<w:{tag} w:id="1"><w:p><w:r><w:t>A useful {tag}.</w:t></w:r></w:p></w:{tag}></w:{tag}s>').encode()
    etree.SubElement(types, "{http://schemas.openxmlformats.org/package/2006/content-types}Override", PartName="/" + part,
        ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml." + tag + "s+xml")
    etree.SubElement(relations, "{http://schemas.openxmlformats.org/package/2006/relationships}Relationship", Id="rId" + tag,
        Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/" + tag + "s", Target=tag + "s.xml")
package["[Content_Types].xml"] = etree.tostring(types, xml_declaration=True, encoding="UTF-8", standalone=True)
package["word/_rels/document.xml.rels"] = etree.tostring(relations, xml_declaration=True, encoding="UTF-8", standalone=True)
package["word/document.xml"] = etree.tostring(document, xml_declaration=True, encoding="UTF-8", standalone=True)
with zipfile.ZipFile(root / "sample.docx", "w", zipfile.ZIP_DEFLATED) as bundle:
    for name, data in package.items():
        bundle.writestr(name, data)

presentation = Presentation()
for title, body in [("Project overview", "Run translation locally.\nKeep documents editable."), ("Next steps", "Review the translation before sharing.")]:
    slide = presentation.slides.add_slide(presentation.slide_layouts[6])
    box = slide.shapes.add_textbox(PptInches(0.7), PptInches(0.4), PptInches(8.5), PptInches(0.8))
    box.text = title
    box.text_frame.paragraphs[0].runs[0].font.size = Pt(30)
    box = slide.shapes.add_textbox(PptInches(0.7), PptInches(1.5), PptInches(5.5), PptInches(1.8))
    box.text = body
    for paragraph in box.text_frame.paragraphs:
        for run in paragraph.runs:
            run.font.size = Pt(22)
    slide.shapes.add_picture(BytesIO(image_bytes), PptInches(6.8), PptInches(1.6), width=PptInches(2.4))
    slide.notes_slide.notes_text_frame.text = "Speaker notes for this slide."
slide = presentation.slides[0]
table = slide.shapes.add_table(2, 2, PptInches(0.7), PptInches(3.8), PptInches(8), PptInches(1.2)).table
for row, values in zip(table.rows, [("Feature", "Status"), ("Translation", "Ready")]):
    for cell, value in zip(row.cells, values):
        cell.text = value
data = CategoryChartData()
data.categories = ["Alpha", "Beta"]
data.add_series("Results", [2, 5])
chart = presentation.slides[1].shapes.add_chart(XL_CHART_TYPE.COLUMN_CLUSTERED, PptInches(0.7), PptInches(3.5), PptInches(7), PptInches(2.8), data).chart
chart.has_title = True
chart.chart_title.text_frame.text = "Quarterly performance"
presentation.save(root / "sample.pptx")

# Adversarial fixtures retain a real Office package, replacing only the body.
with zipfile.ZipFile(root / "sample.docx") as source:
    original = {name: source.read(name) for name in source.namelist()}
ns = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
bodies = {
    "long.docx": '<w:p><w:r><w:t>' + "A long paragraph with enough context. " * 90 + '</w:t></w:r></w:p>',
    "fields.docx": '<w:p><w:r><w:fldChar w:fldCharType="begin"/></w:r></w:p>'
        '<w:p><w:r><w:instrText>TOC</w:instrText></w:r><w:r><w:t>Generated contents</w:t></w:r></w:p>'
        '<w:p><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p>'
        '<w:p><w:r><w:t>Normal paragraph</w:t></w:r></w:p>'
        '<w:ins w:id="1" w:author="Test"><w:p><w:r><w:t>Tracked paragraph</w:t></w:r></w:p></w:ins>',
}
for name, body in bodies.items():
    with zipfile.ZipFile(root / name, "w", zipfile.ZIP_DEFLATED) as out:
        for part, data in original.items():
            if part == "word/document.xml":
                data = f'<?xml version="1.0" encoding="UTF-8"?><w:document {ns}><w:body>{body}</w:body></w:document>'.encode()
            out.writestr(part, data)
with zipfile.ZipFile(root / "images-only.docx", "w", zipfile.ZIP_DEFLATED) as out:
    for part, data in original.items():
        if part.startswith("word/") and part.endswith(".xml"):
            tree = etree.fromstring(data)
            for node in tree.iter(qn("w:t")):
                node.text = None
            data = etree.tostring(tree, xml_declaration=True, encoding="UTF-8", standalone=True)
        out.writestr(part, data)
print("Generated", ", ".join(p.name for p in root.glob("*")))
