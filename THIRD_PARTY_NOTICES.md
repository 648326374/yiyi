# Third-party components

## WinUI and Windows platform

- C++/WinRT supplies the C++ projection for Windows Runtime APIs. The portable package includes its license as `CppWinRT-LICENSE.txt`. [C++/WinRT source and license](https://github.com/microsoft/cppwinrt)
- WinUI 3 uses Windows App SDK. The compact ZIP contains the bootstrap DLL for the shared Windows App Runtime 2.4 x64. The self-contained ZIP ships the runtime files and `WindowsAppSDK-NOTICE.txt`. Both include `WindowsAppSDK-LICENSE.txt`. [Windows App SDK source and license](https://github.com/microsoft/WindowsAppSDK)
- SQLite storage uses the SQLite implementation supplied by Windows in `winsqlite3.dll`. [SQLite copyright](https://www.sqlite.org/copyright.html)

The interface uses system fonts. PDF export bundles the open-source fonts described below.

## Office translation in 0.11.0

- miniz 3.1.0, MIT, copyright RAD Game Tools, Valve Software, Rich Geldreich and Tenacious Software LLC. Vendored sources and SHA256 provenance are in `third_party/miniz`. A generated `miniz_export.h` supplies the static-library export macro. The application links miniz statically for OOXML ZIP reading and writing. Its license ships as `Miniz-LICENSE.txt`. [miniz 3.1.0](https://github.com/richgel999/miniz/tree/3.1.0)
- Word/PPT translation does not require Microsoft Office or LibreOffice. Optional PDF layout preview invokes a user-installed LibreOffice executable; LibreOffice is not bundled. `python-docx`, `python-pptx`, Pillow and lxml are development-only fixture generators and are not runtime dependencies.

## Local OCR in 0.4.0

The portable package includes a separate native CPU OCR worker. It needs no Python installation, GPU runtime or OCR downloads at application startup.

- RapidOcrOnnx 1.2.2, Apache-2.0: https://github.com/RapidAI/RapidOcrOnnx/tree/1.2.2 . Yiyi builds selected sources with a new C ABI bridge, ONNX Runtime 1.23.2, initialized session pointers and bounded thread pools. The patches and build instructions are in `tools/ocr-native/CMakeLists.txt` and `scripts/setup-ocr.ps1`.
- OpenCV 4.6.0, Apache-2.0, and its bundled image codec dependencies are linked into `YiyiOcr.dll`; their supplied notices are copied into `ocr/licenses`.
- ONNX Runtime 1.23.2, MIT, copyright Microsoft and contributors. Its license and third-party notices are in `ocr/licenses`. https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.23.2
- PP-OCRv5 mobile Chinese/English detection and recognition models, matching character dictionary, and `ch_ppocr_mobile_v2.0_cls`: upstream model weights copyright Baidu and/or the applicable PaddleOCR rights holders, Apache-2.0. ONNX conversions are distributed by RapidAI. Model provenance, URLs and SHA256 values are pinned in `scripts/ocr-dependencies.json` and the shipped `ocr/dependencies.json`. Official PP-OCRv5 model cards and the PaddleOCR license ship in `ocr/licenses`; RapidOCR's model notice additionally covers the legacy classifier. These OCR weights are separate from the user's translation/vision model.
- Microsoft Visual C++ runtime DLLs required by ONNX Runtime are deployed from the Visual Studio redistributable directory under Microsoft's redistribution terms: https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files . They are not covered by the Apache or MIT licenses above.

7-Zip 25.01 is a build-time extractor only (https://www.7-zip.org/license.txt); neither the extractor nor downloaded source archives are included in the portable application. Development probes and benchmark images are not shipped. No third-party sample images are redistributed.

## PDF translation in 0.5.0

- PDFium, Chromium revision 8138, dynamically loaded in a separate worker. Prebuilt x64 DLL and public headers from https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium/8138 . PDFium is BSD-licensed and includes separately licensed dependencies. Its supplied LICENSE and all dependency notices are shipped in `pdf/licenses`. Source: https://pdfium.googlesource.com/pdfium/ . Download and checksum are pinned in `scripts/pdf-dependencies.json` (also shipped under `pdf`).
- **Yiyi PDF Sans Regular**, derived from Noto Sans CJK SC Regular at https://github.com/notofonts/noto-cjk/tree/f8d157532fbfaeda587e826d4cd5b21a49186f7c , copyright the Noto Project Authors and upstream holders, licensed under the SIL Open Font License 1.1. The original copyright and OFL text are retained in `pdf/licenses/NotoSans-LICENSE.txt`. The modified font remains under OFL. `tools/prepare_pdf_font.py` renames family/full/PostScript names and removes Unicode cmap aliases in U+2E80–U+2FDF so ordinary ideographs export with correct searchable Unicode. It retains Unicode characters and layout-feature closure, drops unreachable glyphs, and duplicates remaining Unicode aliases into distinct glyph IDs for reliable PDF text extraction. Glyph outlines remain unchanged. The exact source and resulting font hashes are pinned. Translated PDFs embed font subsets.
- **Yiyi PDF Serif Regular** (0.7.0), derived from Noto Serif CJK SC Regular at the same pinned Noto CJK revision, under SIL Open Font License 1.1. The corresponding license is shipped as `pdf/licenses/NotoSerif-LICENSE.txt`. The same renaming and Unicode cmap preparation is applied, with no outline changes. Both source and prepared font hashes are pinned in `scripts/pdf-dependencies.json`.
- PDF scan layout detection reuses the bundled RapidOCR detection model and runtime listed above. The detection-only C ABI loads `det.onnx` and returns quadrilateral coordinates; the configured vision service performs transcription. No additional OCR recognizer or Windows OCR dependency is introduced.
- FontTools 4.60.1 (MIT): https://github.com/fonttools/fonttools/tree/4.60.1 . A verified pure-Python wheel is used **only at build time** for the font conversion; neither Python nor FontTools ships in the portable app. Its license is included in the downloaded wheel.

## Large PDF export in 0.9.0

- qpdf 12.2.0, Apache-2.0, copyright Jay Berkenbilt and Manfred Holger: https://github.com/qpdf/qpdf/releases/tag/v12.2.0 . The official mingw64 `qpdf30.dll` is shipped unchanged as `pdf/qpdf/qpdf.dll` and loaded through its C API. It combines translated page contents/resources while retaining the original page identities, catalog, bookmarks, links and annotations. The source release, official binary URL and SHA256 are pinned in `scripts/pdf-dependencies.json`; `scripts/setup-pdf-merge.ps1` prepares the runtime. Its LICENSE and NOTICE are included in `pdf/licenses/qpdf-*`.
- This qpdf binary statically includes zlib 1.3.1 (zlib license), Independent JPEG Group libjpeg 9f (IJG license), and OpenSSL 3.5.0 (Apache-2.0). Their licenses ship in `pdf/licenses/qpdf-zlib-LICENSE.txt`, `qpdf-IJG-LICENSE.txt`, and `qpdf-OpenSSL-LICENSE.txt`. This software is based in part on the work of the Independent JPEG Group. OpenSSL is copyright the OpenSSL Project Authors. The native qpdf crypto implementation and its notices are covered in `qpdf-NOTICE.md`. [OpenSSL](https://www.openssl.org/)
- The official qpdf release supplies `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, and `libwinpthread-1.dll`. These binaries ship alongside qpdf in `pdf/qpdf/`. The GCC license, GCC Runtime Library Exception, and winpthreads license ship under `pdf/licenses/`. [GCC runtime licensing](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html) · [MinGW-w64](https://www.mingw-w64.org/)
