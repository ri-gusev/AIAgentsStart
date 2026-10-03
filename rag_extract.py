"""Local text extraction only; never executes content embedded in documents."""
import json
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET

MAX_TEXT = 2_000_000


def extract(path):
    extension = path.suffix.lower()
    if extension in (".txt", ".md"):
        data = path.read_bytes()
        encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        try:
            text = data.decode(encoding)
        except UnicodeDecodeError:
            text = data.decode("cp1251")
        if "\0" in text:
            raise ValueError("Файл не является текстовым документом.")
        return [{"text": text, "page": 0, "type": "markdown" if extension == ".md" else "text"}]
    if extension == ".pdf":
        from pypdf import PdfReader
        reader = PdfReader(path)
        if reader.is_encrypted and not reader.decrypt(""):
            raise ValueError("PDF защищён паролем.")
        if len(reader.pages) > 1000:
            raise ValueError("PDF содержит слишком много страниц (максимум 1000).")
        result = []
        total = 0
        for number, page in enumerate(reader.pages, 1):
            stream = page.get_contents()
            if stream and len(stream.get_data()) > 10_000_000:
                raise ValueError("Страница PDF слишком велика для извлечения текста.")
            text = page.extract_text() or ""
            total += len(text)
            if total > MAX_TEXT:
                raise ValueError("Слишком много текста в документе.")
            if text.strip():
                result.append({"text": text, "page": number, "type": "text"})
        if not result:
            raise ValueError("В PDF нет текстового слоя. Для скана требуется OCR.")
        return result
    if extension == ".docx":
        with zipfile.ZipFile(path) as archive:
            info = archive.getinfo("word/document.xml")
            if info.file_size > 16_000_000:
                raise ValueError("Слишком большой DOCX.")
            root = ET.fromstring(archive.read(info))
        ns = {"w": "http://schemas.openxmlformats.org/wordprocessingml/2006/main"}
        lines = []
        for paragraph in root.findall(".//w:p", ns):
            text = "".join(node.text or "" for node in paragraph.findall(".//w:t", ns))
            style = paragraph.find("w:pPr/w:pStyle", ns)
            if style is not None:
                value = style.get("{" + ns["w"] + "}val", "")
                if value.lower().startswith("heading") and value[-1:].isdigit():
                    text = "#" * min(6, int(value[-1]) or 1) + " " + text
            lines.append(text)
        return [{"text": "\n\n".join(lines), "page": 0, "type": "markdown"}]
    raise ValueError("Поддерживаются TXT, MD, PDF и DOCX.")


if __name__ == "__main__":
    try:
        parts = extract(Path(sys.argv[1]))
        if sum(len(part["text"]) for part in parts) > MAX_TEXT:
            raise ValueError("Слишком много текста в документе.")
        if not any(part["text"].strip() for part in parts):
            raise ValueError("В документе нет текста.")
        result = {"parts": parts}
    except ImportError:
        result = {"error": "Для PDF требуется pypdf из requirements-rag.txt."}
    except ValueError as error:
        result = {"error": str(error)}
    except Exception:
        result = {"error": "Не удалось прочитать документ: повреждённый или неподдерживаемый файл."}
    Path(sys.argv[2]).write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
