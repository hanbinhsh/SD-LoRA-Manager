"""Maintain Qt catalogs for fixed UI text and explicitly marked C++ text.

Run without options to rebuild TS files, or --check to verify coverage. The
one-time --mark-display-text migration only touches literal presentation text.
"""
import argparse
import ast
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DIRECTORIES = ("windows", "pages", "tools", "dialogs", "widgets")
HAN = re.compile(r"[\u3400-\u9fff]")
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|R"(?P<delim>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delim)"|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|::|->|[^\s]', re.S)


def tokens(text):
    return [m for m in TOKEN.finditer(text) if not m.group().startswith(("//", "/*"))]


def literal(token):
    raw = token.group()
    if raw.startswith('R"'):
        delim = raw[2:raw.index("(")]
        return raw[raw.index("(") + 1:-(len(delim) + 2)]
    raw = re.sub(r'^(u8|u|U|L)(?=")', "", raw)
    if not raw.startswith('"'):
        return None
    try:
        return ast.literal_eval(raw)
    except (SyntaxError, ValueError):
        return None


def call_ranges(ts):
    stack, calls = [], []
    for i, token in enumerate(ts):
        value = token.group()
        if value in ("(", "{", "["):
            stack.append((i, []))
        elif value == "," and stack:
            stack[-1][1].append(i)
        elif value in (")", "}", "]") and stack:
            start, commas = stack.pop()
            if ts[start].group() == "(" and start > 0 and re.fullmatch(r"[A-Za-z_]\w*", ts[start - 1].group()):
                cuts = [start] + commas + [i]
                args = [(cuts[j] + 1, cuts[j + 1]) for j in range(len(cuts) - 1)]
                calls.append((start, i, ts[start - 1].group(), args))
    return calls


def visible_args(ts, call):
    start, end, name, args = call
    simple = {"setText", "setTitle", "setWindowTitle", "setPlaceholderText", "setToolTip", "setStatusTip", "setWhatsThis", "setInformativeText", "setDetailedText", "showMessage", "setHeaderLabels", "setHorizontalHeaderLabels", "setFormat", "setDialogTitle", "setHintText", "setStatusText", "setThemeStatus", "setCivitaiApiStatus"}
    constructors = {"QLabel", "QPushButton", "QToolButton", "QCheckBox", "QRadioButton", "QGroupBox", "QAction", "QMenu", "QCommandLinkButton"}
    if name in simple:
        return args[:2] if name == "setText" else args[:1]
    if name in constructors:
        return args[:2] if args and any(t.group() == "QIcon" for t in ts[args[0][0]:args[0][1]]) else args[:1]
    if name in {"addTab", "insertTab", "setTabText"}:
        return args[1:3]
    if name in {"addAction", "addMenu", "addItem", "addRow", "addButton", "buildSubmenu", "makeToolPlaceholder"}:
        return args[1:2] if args and any(t.group() == "QIcon" for t in ts[args[0][0]:args[0][1]]) else args[:1]
    owner = ts[start - 3].group() if start >= 3 and ts[start - 2].group() == "::" else ""
    if owner in {"QMessageBox", "QInputDialog"}:
        return args[1:3]
    if owner == "QFileDialog":
        return args[1:2] + args[3:4]
    if start >= 2 and ts[start - 2].group() == "QMessageBox":
        return args[1:3]
    if name in {"setAiTagTranslateDownloadState", "showPendingLocalModelDetail"}:
        return args[1:2]
    return []


def mark_display_text(path):
    original = path.read_text(encoding="utf-8-sig")
    text = original
    text = re.sub(r'\[\s*AppText::tr\(("(?:\\.|[^"\\])*")\)\s*\]', r'[\1]', text)
    ts = tokens(text)
    calls = call_ranges(ts)
    brackets, index_ranges = [], []
    for i, token in enumerate(ts):
        if token.group() == "[":
            brackets.append(i)
        elif token.group() == "]" and brackets:
            index_ranges.append((brackets.pop(), i))
    selected = set()
    for call in calls:
        ranges = call[3][:1] if call[2] == "updateStatus" and path.name == "llmpromptwidget.cpp" else visible_args(ts, call)
        for begin, end in ranges:
            for i in range(begin, end):
                value = literal(ts[i])
                if value is None or not re.search(r"[A-Za-z\u3400-\u9fff]", value):
                    continue
                if any(a < i < b for a, b in index_ranges):
                    continue
                containers = [c for c in calls if c[0] < i < c[1]]
                nearest = max(containers, key=lambda c: c[0])
                if nearest != call and nearest[2] not in {"QString", "QStringLiteral", "fromUtf8"}:
                    continue
                selected.add(i)
    for i, token in enumerate(ts):
        value = literal(token)
        if value and default_translation(value)["en_US"] != value:
            if not any(a < i < b and name == "tr" for a, b, name, _ in calls):
                selected.add(i)
    replacements, consumed = [], set()
    for i in sorted(selected):
        if i in consumed:
            continue
        first, last = i, i
        while first > 0 and literal(ts[first - 1]) is not None:
            first -= 1
        while last + 1 < len(ts) and literal(ts[last + 1]) is not None:
            last += 1
        consumed.update(range(first, last + 1))
        source = "".join(literal(t) for t in ts[first:last + 1])
        if not HAN.search(source) and " / " not in source:
            # Standard English captions are useful in the Chinese catalog too.
            if len(source) > 100 or source.startswith(("http", "<", "#")):
                continue
        begin, end = ts[first].start(), ts[last].end()
        if first >= 2 and ts[first - 1].group() == "(" and ts[first - 2].group() in {"QString", "QStringLiteral"} and ts[last + 1].group() == ")":
            begin = ts[first - 2].start()
            end = ts[last + 1].end()
            replacement = "AppText::tr(" + text[ts[first].start():ts[last].end()] + ")"
        else:
            replacement = "AppText::tr(" + text[begin:end] + ")"
        replacements.append((begin, end, replacement))
    for begin, end, value in sorted(set(replacements), reverse=True):
        text = text[:begin] + value + text[end:]
    if text != original:
        if '#include "utils/applanguage.h"' not in text:
            text = '#include "utils/applanguage.h"\n' + text
        path.write_text(text, encoding="utf-8", newline="\n")


def sources():
    result = set()
    for directory in DIRECTORIES:
        for path in (ROOT / directory).glob("*.ui"):
            for value in ET.parse(path).iter("string"):
                if value.get("notr") != "true" and value.text:
                    result.add(value.text)
        for path in (ROOT / directory).glob("*.*"):
            if path.suffix not in {".cpp", ".h"}:
                continue
            ts = tokens(path.read_text(encoding="utf-8-sig"))
            for start, end, name, args in call_ranges(ts):
                if name not in {"tr", "translate"} or not args:
                    continue
                begin, finish = args[1 if name == "translate" and len(args) > 1 else 0]
                values = [literal(t) for t in ts[begin:finish]]
                if values and all(v is not None for v in values):
                    result.add("".join(values))
    return sorted(result)


def legacy_translation(source):
    # Split only actual bilingual captions, not paths, fractions or alternatives.
    parts = re.split(r"\s+/\s+", source)
    if len(parts) == 2 and bool(HAN.search(parts[0])) != bool(HAN.search(parts[1])):
        zh, en = (parts[0], parts[1]) if HAN.search(parts[0]) else (parts[1], parts[0])
        prefix = re.match(r"^[^A-Za-z\u3400-\u9fff]*", source).group()
        if prefix.strip():
            zh = prefix + zh.lstrip(prefix)
            en = prefix + en.lstrip(prefix)
        return {"zh_CN": zh, "en_US": en}
    return {"zh_CN": source, "en_US": source}


def default_translation(source):
    parts = re.split(r"\s+/\s+", source)
    if len(parts) != 2 or "\n" in source or "<" in source or ">" in source:
        return {"zh_CN": source, "en_US": source}
    english = parts[1] if HAN.search(parts[0]) else parts[0]
    if not re.search(r"[A-Za-z]", english):
        return {"zh_CN": source, "en_US": source}
    if any(part.count("(") != part.count(")") for part in parts):
        return {"zh_CN": source, "en_US": source}
    if set(re.findall(r"%[1-9][0-9]?", parts[0])) != set(re.findall(r"%[1-9][0-9]?", parts[1])):
        return {"zh_CN": source, "en_US": source}
    return legacy_translation(source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mark-display-text", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--missing", action="store_true")
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--count", type=int, default=150)
    parser.add_argument("--import-translations", type=Path)
    parser.add_argument("--import-source", type=Path)
    args = parser.parse_args()
    if args.mark_display_text:
        for directory in DIRECTORIES:
            for path in (ROOT / directory).glob("*.*"):
                if path.suffix in {".cpp", ".h"}:
                    mark_display_text(path)
    path = ROOT / "translations" / "catalog.json"
    catalog = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    values = sources()
    if args.import_source:
        for source, translation in json.loads(args.import_source.read_text(encoding="utf-8")).items():
            catalog[source] = (translation if isinstance(translation, dict)
                               else {"zh_CN": default_translation(source)["zh_CN"], "en_US": translation})
    missing = []
    for source in values:
        entry = catalog.get(source, default_translation(source))
        if HAN.search(entry["en_US"]):
            missing.append(source)
        catalog[source] = entry
    if args.import_translations:
        translated = json.loads(args.import_translations.read_text(encoding="utf-8"))
        for index, english in translated.items():
            catalog[missing[int(index)]]["en_US"] = english
        missing = [source for source in missing if HAN.search(catalog[source]["en_US"])]
    if args.missing:
        print(json.dumps({i: value for i, value in enumerate(missing) if args.start <= i < args.start + args.count}, ensure_ascii=False, indent=2))
        return
    if args.check:
        absent = [v for v in values if v not in json.loads(path.read_text(encoding="utf-8"))]
        placeholders = re.compile(r"%[1-9][0-9]?")
        invalid = [source for source, entry in catalog.items()
                   if any(set(placeholders.findall(source)) != set(placeholders.findall(value)) for value in entry.values())]
        if missing or absent or invalid:
            print(f"{len(missing)} untranslated English captions; {len(absent)} uncatalogued captions")
            print("Invalid placeholders:", json.dumps(invalid, ensure_ascii=False))
            raise SystemExit(1)
        print(f"{len(values)} UI captions covered")
        return
    path.parent.mkdir(exist_ok=True)
    path.write_text(json.dumps(dict(sorted(catalog.items())), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for language in ("zh_CN", "en_US"):
        root = ET.Element("TS", version="2.1", language=language)
        context = ET.SubElement(root, "context")
        ET.SubElement(context, "name").text = "AppText"
        for source, entry in sorted(catalog.items()):
            message = ET.SubElement(context, "message")
            ET.SubElement(message, "source").text = source
            ET.SubElement(message, "translation").text = entry[language]
        ET.indent(root, space="  ")
        ET.ElementTree(root).write(path.with_name(f"app_{language}.ts"), encoding="utf-8", xml_declaration=True)
    print(f"Generated {len(catalog)} messages; {len(missing)} English translations remaining")


if __name__ == "__main__":
    main()
