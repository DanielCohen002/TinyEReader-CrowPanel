#!/usr/bin/env python3
"""
Convert an EPUB to a plain .txt file for PocketReader, inserting a form-feed
(0x0C) character at each detected chapter boundary. PocketReader's firmware
scans the uploaded .txt for these bytes to build its chapter list, so the
Top/Bottom buttons can jump between chapters while reading. Form feed is
invisible in the rendered text -- it's stripped out, not printed.

Usage:
    python epub_to_txt.py mybook.epub [output.txt]

If the EPUB uses real heading tags (<h1>/<h2>/<h3>) for chapter titles,
those become the chapter markers. If too few headings are found, falls back
to one marker per file in the book's internal reading order (usually close
to one-per-chapter anyway, since most EPUBs split a file per chapter).

Requires only the Python standard library -- nothing to install.
"""
import sys
import os
import re
import zipfile
import urllib.parse
from html.parser import HTMLParser
from xml.etree import ElementTree as ET

HEADING_TAGS = {"h1", "h2", "h3"}
SKIP_TAGS = {"script", "style", "head"}
BLOCK_TAGS = {"p", "div", "li", "tr", "blockquote"}


class ChapterAwareTextExtractor(HTMLParser):
    """Strips HTML tags to plain text, tracking the text-offset of each
    heading tag so callers can insert chapter markers there."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.chunks = []
        self.heading_offsets = []
        self.skip_depth = 0
        self.at_line_start = True
        self.length = 0

    def _emit(self, text):
        if not text:
            return
        self.chunks.append(text)
        self.length += len(text)
        self.at_line_start = text.endswith("\n")

    def _break_paragraph(self):
        if not self.at_line_start:
            self._emit("\n\n")

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag in SKIP_TAGS:
            self.skip_depth += 1
            return
        if tag in HEADING_TAGS:
            self._break_paragraph()
            self.heading_offsets.append(self.length)
        elif tag in BLOCK_TAGS:
            self._break_paragraph()

    def handle_startendtag(self, tag, attrs):
        if tag.lower() == "br":
            self._break_paragraph()

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag in SKIP_TAGS:
            self.skip_depth = max(0, self.skip_depth - 1)
            return
        if tag in HEADING_TAGS or tag in BLOCK_TAGS:
            self._break_paragraph()

    def handle_data(self, data):
        if self.skip_depth > 0:
            return
        text = re.sub(r"[ \t\r\f\v]+", " ", data)
        if text.strip() == "":
            return
        if self.at_line_start:
            text = text.lstrip(" ")
        self._emit(text)

    def get_text(self):
        return "".join(self.chunks).strip()


def read_zip_bytes(zf, path):
    with zf.open(path) as f:
        return f.read()


def read_zip_text(zf, path):
    return read_zip_bytes(zf, path).decode("utf-8", errors="replace")


def find_opf_path(zf):
    # container.xml always lives at this fixed path per the EPUB spec.
    container = read_zip_bytes(zf, "META-INF/container.xml")
    root = ET.fromstring(container)
    ns = {"c": "urn:oasis:names:tc:opendocument:xmlns:container"}
    rootfile = root.find(".//c:rootfile", ns)
    return rootfile.attrib["full-path"]


def get_spine_files(zf, opf_path):
    opf_bytes = read_zip_bytes(zf, opf_path)
    root = ET.fromstring(opf_bytes)
    ns = {"opf": "http://www.idpf.org/2007/opf"}

    manifest = {}
    for item in root.findall(".//opf:manifest/opf:item", ns):
        manifest[item.attrib["id"]] = urllib.parse.unquote(item.attrib["href"])

    opf_dir = os.path.dirname(opf_path)
    spine_files = []
    for itemref in root.findall(".//opf:spine/opf:itemref", ns):
        href = manifest.get(itemref.attrib.get("idref"))
        if not href:
            continue
        path = href if not opf_dir else opf_dir + "/" + href
        path = os.path.normpath(path).replace("\\", "/")
        spine_files.append(path)
    return spine_files


def convert(epub_path, txt_path):
    with zipfile.ZipFile(epub_path) as zf:
        opf_path = find_opf_path(zf)
        spine_files = get_spine_files(zf, opf_path)

        if not spine_files:
            raise SystemExit("Could not find any readable content in this EPUB.")

        file_texts = []
        file_heading_offsets = []
        for path in spine_files:
            try:
                html = read_zip_text(zf, path)
            except KeyError:
                continue
            parser = ChapterAwareTextExtractor()
            parser.feed(html)
            text = parser.get_text()
            if text:
                file_texts.append(text)
                file_heading_offsets.append(parser.heading_offsets)

    if not file_texts:
        raise SystemExit("Could not extract any text from this EPUB.")

    total_headings = sum(len(offs) for offs in file_heading_offsets)

    pieces = []
    marker_count = 0
    if total_headings >= 2:
        # Real headings found -- use those as chapter boundaries. Skip only
        # the very first heading of the very first file (the book's own
        # opening chapter title), not the first heading of every file --
        # each subsequent file's first heading IS a real chapter boundary.
        is_first_heading_overall = True
        for text, offsets in zip(file_texts, file_heading_offsets):
            cursor = 0
            for off in offsets:
                if is_first_heading_overall:
                    is_first_heading_overall = False
                    continue
                pieces.append(text[cursor:off])
                pieces.append("\f")
                marker_count += 1
                cursor = off
            pieces.append(text[cursor:])
            pieces.append("\n\n")
    else:
        # No usable headings -- fall back to one marker per spine file.
        for i, text in enumerate(file_texts):
            if i > 0:
                pieces.append("\f")
                marker_count += 1
            pieces.append(text)
            pieces.append("\n\n")

    final_text = "".join(pieces).strip() + "\n"

    with open(txt_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(final_text)

    print(f"Wrote {txt_path} ({len(final_text)} bytes, {marker_count + 1} chapters detected)")
    if marker_count == 0:
        print("Warning: no chapter boundaries detected -- chapter skip will have nothing to jump between.")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    epub_path = sys.argv[1]
    txt_path = sys.argv[2] if len(sys.argv) >= 3 else os.path.splitext(epub_path)[0] + ".txt"

    convert(epub_path, txt_path)


if __name__ == "__main__":
    main()
