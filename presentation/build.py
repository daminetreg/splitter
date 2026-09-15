#!/usr/bin/env python3
"""Build the standalone Markdown-authored presentation.

The format is deliberately small: front matter, Markdown headings/paragraphs,
fenced code, and ::: directives documented in presentation/README.md.
"""
from __future__ import annotations

import argparse
import base64
import html
import json
import math
import os
import re
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HERE = Path(__file__).resolve().parent
KNOWN = {"flow", "fission", "cards", "tree", "rationale", "metrics", "bars", "legend",
         "callout", "tiny", "popup", "map",
         "section-map", "sizes", "binary-trace", "paths", "single-bars",
         "code-columns", "semantic", "logic", "flag", "logo", "mermaid", "mermaid-columns", "list"}

class SourceError(Exception):
    def __init__(self, path, line, message):
        super().__init__(f"{path}:{line}: {message}")

@dataclass
class Block:
    kind: str
    arg: str
    body: str
    line: int

@dataclass
class Slide:
    path: Path
    meta: dict
    blocks: list[Block]

def fail(path, line, message):
    raise SourceError(path, line, message)

def frontmatter(path, text):
    if not text.startswith("---\n"):
        fail(path, 1, "expected opening front matter ---")
    end = text.find("\n---\n", 4)
    if end < 0:
        fail(path, 1, "front matter is not closed")
    meta = {}
    for offset, row in enumerate(text[4:end].splitlines(), 2):
        if not row.strip():
            continue
        if ":" not in row:
            fail(path, offset, "front matter entries must be key: value")
        k, v = row.split(":", 1)
        if not re.fullmatch(r"[a-z][a-z0-9_-]*", k):
            fail(path, offset, f"invalid front matter key {k!r}")
        meta[k] = v.strip()
    return meta, text[end + 5:], text[:end + 5].count("\n")

def parse_markdown(path: Path, text: str) -> Slide:
    meta, body, base = frontmatter(path, text)
    for required in ("chapter", "notes"):
        if not meta.get(required):
            fail(path, 2, f"missing required front matter {required!r}")
    blocks, lines, i = [], body.splitlines(), 0
    while i < len(lines):
        line, n = lines[i], base + i + 1
        if not line.strip():
            i += 1; continue
        if line.startswith("```"):
            lang = line[3:].strip()
            i += 1; start, out = i, []
            while i < len(lines) and not lines[i].startswith("```"):
                out.append(lines[i]); i += 1
            if i == len(lines):
                fail(path, n, "unclosed fenced code block")
            blocks.append(Block("code", lang, "\n".join(out), base + start + 1))
            i += 1; continue
        if line.startswith(":::"):
            bits = line[3:].strip().split(maxsplit=1)
            if not bits or bits[0] not in KNOWN:
                fail(path, n, f"unknown directive {bits[0] if bits else '(empty)'!r}")
            kind, arg = bits[0], bits[1] if len(bits) == 2 else ""
            i += 1; out = []
            while i < len(lines) and lines[i] != ":::":
                out.append(lines[i]); i += 1
            if i == len(lines):
                fail(path, n, f"unclosed {kind} directive")
            blocks.append(Block(kind, arg, "\n".join(out), n))
            i += 1; continue
        match = re.match(r"^(#{1,3})\s+(.+)$", line)
        if match:
            # A Markdown hard break is a deliberate presentation line break,
            # including when it continues a heading.
            out = [match.group(2)]
            i += 1
            while out[-1].endswith("  ") and i < len(lines) and lines[i].strip():
                out.append(lines[i]); i += 1
            blocks.append(Block("heading", str(len(match.group(1))), "\n".join(out), n))
            continue
        out = [line]
        i += 1
        while i < len(lines) and lines[i].strip() and not lines[i].startswith(("```", ":::","#")):
            out.append(lines[i]); i += 1
        prose = "\n".join(out)
        if re.search(r"<[A-Za-z][^>]*>", prose):
            fail(path, n, "raw HTML is not allowed; use Markdown or a directive")
        blocks.append(Block("paragraph", "", prose, n))
    return Slide(path, meta, blocks)

def lines(block, minimum=1):
    values = [x[2:] for x in block.body.splitlines() if x.startswith("- ")]
    if len(values) < minimum:
        fail(block.path if hasattr(block, "path") else "slide", block.line,
             f"{block.kind} needs at least {minimum} '- ' rows")
    return values

def inline(text):
    """Escape input first, then permit emphasis and {tone} semantic spans."""
    text = html.escape(text, quote=False)
    text = text.replace("  \n", "<br>")
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"\{(accent|violet|split|red)\}(.+?)\{/\1\}",
                  r'<span class="\1">\2</span>', text)
    return text

def pipe_rows(block, count, labels):
    result = []
    for raw in [x[2:] for x in block.body.splitlines() if x.startswith("- ")]:
        parts = [p.strip() for p in raw.split("|")]
        if len(parts) != count:
            fail(getattr(block, "path", "slide"), block.line, f"{block.kind} row needs {' | '.join(labels)}")
        result.append(parts)
    if not result:
        fail(getattr(block, "path", "slide"), block.line, f"{block.kind} has no rows")
    return result

def popup_attrs(reference):
    if not reference:
        return ""
    return f' tabindex="0" role="button" data-popup="{html.escape(reference, quote=True)}"'

def render_block(block, snippets):
    k, a, b = block.kind, block.arg, block.body
    if k == "semantic":
        labels = [x.strip() for x in a.split("|")]
        parts = re.split(r"^---$", b, flags=re.M)
        if len(labels) != 2 or not all(labels) or len(parts) != 2:
            fail(block.path, block.line, "semantic needs two labels separated by | and two fenced blocks separated by ---")
        columns = []
        for index, part in enumerate(parts):
            match = re.fullmatch(r"\s*```diff\n(.*?)\n```\s*", part, re.S)
            if not match:
                fail(block.path, block.line, "semantic contents must be fenced diff code")
            rows = []
            for row in match.group(1).splitlines():
                tone = {"+": "diffline", "=": "same"}.get(row[:1])
                text = html.escape(row[1:] if tone else row)
                rows.append(f'<span class="{tone}">{text}</span>' if tone else text)
            code = "\n".join(rows)
            tone = "violet" if index == 0 else "accent"
            columns.append(f'<div><h3 class="{tone}">{inline(labels[index])}</h3><pre class="code">{code}</pre></div>')
        return '<div class="semantic">' + '<div class="arrow">⇄</div>'.join(columns) + "</div>"
    if k == "heading":
        tag = "h1" if a == "1" else ("h2" if a == "2" else "h3")
        return f"<{tag}>{inline(b)}</{tag}>"
    if k == "paragraph":
        return f'<p class="lead">{inline(b)}</p>'
    if k == "code":
        return f'<pre class="code" data-language="{html.escape(a, quote=True)}">{html.escape(b)}</pre>'
    if k == "code-columns":
        parts = re.split(r"^---$", b, flags=re.M)
        if len(parts) != 2:
            fail(block.path, block.line, "code-columns needs two fenced code blocks separated by ---")
        code = []
        for part in parts:
            match = re.fullmatch(r"\s*```([^\n]*)\n(.*?)\n```\s*", part, re.S)
            if not match: fail(block.path, block.line, "code-columns contents must be fenced code")
            language = f' data-language="{html.escape(match.group(1), quote=True)}"' if match.group(1) else ""
            code.append(f'<pre class="code"{language}>{html.escape(match.group(2))}</pre>')
        return '<div class="grid two">' + "".join(code) + "</div>"
    if k in ("mermaid", "mermaid-columns"):
        # Pre-rendered by render-mermaid.py, keyed on the block's text, so the deck stays
        # one offline file. The source stays here, in the slide, as the thing to edit.
        # `mermaid-columns` holds two diagrams separated by `---`, side by side.
        import hashlib
        sources = re.split(r"^---$", b, flags=re.M) if k == "mermaid-columns" else [b]
        if k == "mermaid-columns" and len(sources) != 2:
            fail(block.path, block.line, "mermaid-columns needs two diagrams separated by ---")
        figures = []
        for source in sources:
            digest = hashlib.sha1(source.strip().encode()).hexdigest()[:12]
            svg = HERE / "diagrams" / (digest + ".svg")
            if not svg.exists():
                fail(block.path, block.line,
                     f"mermaid diagram not rendered yet ({digest}); run presentation/render-mermaid.py")
            figures.append(f'<figure class="mermaid-figure">{svg.read_text()}</figure>')
        if k == "mermaid":
            return figures[0]
        return '<div class="grid two mermaid-columns">' + "".join(figures) + "</div>"
    if k == "list":
        items = "".join(f"<li>{inline(x)}</li>" for x in lines(block))
        return f'<ul class="bullets">{items}</ul>'
    if k == "logo":
        # The deck's one embedded EngFlow SVG; the runtime fills src from the footer logo.
        return '<img class="logo-large" alt="EngFlow" data-logo="engflow">'
    if k == "tree":
        return f'<div class="tree">{inline(b)}</div>'
    if k in ("rationale", "callout", "tiny", "flag"):
        return f'<div class="{k}">{inline(b)}</div>'
    if k == "flow":
        rows = []
        for raw in [x[2:] for x in b.splitlines() if x.startswith("- ")]:
            parts = [p.strip() for p in raw.split("|")]
            if len(parts) not in (2, 3): fail(block.path, block.line, "flow row needs label | tone | optional popup reference")
            if len(parts) == 3 and parts[2] and parts[2] not in snippets:
                fail(block.path, block.line, f"unknown snippet reference {parts[2]!r}")
            rows.append(parts)
        if not rows: fail(block.path, block.line, "flow has no rows")
        nodes = [
            f'<div class="node {html.escape(row[1])} {"file" if len(row)==3 and row[2] else ""}" '
            f'{popup_attrs(row[2] if len(row)==3 else "")}>{inline(row[0]).replace(chr(92)+"n","<br>")}</div>'
            for row in rows]
        merge = 1
        if a:
            match = re.fullmatch(r"merge=(\d+)", a)
            if not match or not 1 <= int(match[1]) <= len(nodes):
                fail(block.path, block.line, "flow argument must be merge=N for N sibling inputs")
            merge = int(match[1])
        return '<div class="flow">' + '<div class="arrow">→</div>'.join(
            ["".join(nodes[:merge]), *nodes[merge:]]) + "</div>"
    if k == "fission":
        # One whole on the first row, the pieces it breaks into after it: one -> many.
        rows = pipe_rows(block, 2, ["label", "tone"])
        if len(rows) < 2: fail(block.path, block.line, "fission needs the whole on the first row and at least one piece after it")
        atom, *pieces = [f'<div class="node {html.escape(tone)}">{inline(label).replace(chr(92)+"n","<br>")}</div>'
                         for label, tone in rows]
        return f'<div class="fission"><div class="atom">{atom}</div><div class="arrow">→</div><div class="pieces">{"".join(pieces)}</div></div>'
    if k == "logic":
        rows = pipe_rows(block, 3, ["left node", "decision", "right node"])
        if len(rows) != 1: fail(block.path, block.line, "logic has exactly one row")
        left, decision, right = rows[0]
        left, decision, right = [inline(x).replace("\\n", "<br>") for x in (left, decision, right)]
        return f'<div class="logic"><div class="node">{left}</div><div class="decision">{decision}</div><div class="node split">{right}</div></div>'
    if k == "cards":
        cols = a or "2"
        if cols not in ("2", "3"): fail(block.path, block.line, "cards argument is 2 or 3")
        rows = []
        for raw in [x[2:] for x in b.splitlines() if x.startswith("- ")]:
            parts = [p.strip() for p in raw.split("|")]
            if len(parts) not in (3, 4): fail(block.path, block.line, "cards row needs title | tone | text | optional popup reference")
            if len(parts) == 4 and parts[3] and parts[3] not in snippets: fail(block.path, block.line, f"unknown snippet reference {parts[3]!r}")
            rows.append(parts)
        if not rows: fail(block.path, block.line, "cards has no rows")
        return '<div class="grid ' + ("two" if cols == "2" else "three") + '">' + "".join(
            f'<div class="card {"file" if len(row)==4 and row[3] else ""}" '
            f'{popup_attrs(row[3] if len(row)==4 else "")}>'
            f'<h3 class="{html.escape(row[1])}">{inline(row[0])}</h3><p>{inline(row[2])}</p></div>'
            for row in rows) + "</div>"
    if k == "metrics":
        cols = a or "3"
        rows = pipe_rows(block, 3, ["value", "tone", "label"])
        return '<div class="grid ' + ("two" if cols == "2" else "three") + '">' + "".join(
            f'<div class="card"><div class="metric {html.escape(t)}">{inline(value)}</div><div class="label">{inline(label)}</div></div>'
            for value,t,label in rows) + "</div>"
    if k == "popup":
        if a not in snippets: fail(block.path, block.line, f"unknown snippet reference {a!r}")
        title = b.strip() or snippets[a][0]
        return f'<div class="card file" tabindex="0" role="button" data-popup="{html.escape(a)}"><h3>{inline(title)}</h3><p>{inline(snippets[a][2])}</p></div>'
    if k == "map":
        rows = pipe_rows(block, 3, ["name", "detail", "tone"])
        return '<div class="map">' + "".join(
            f'<div class="obj {html.escape(t)}">{inline(name)}<br><small>{inline(detail)}</small></div>'
            for name,detail,t in rows) + "</div>"
    if k == "legend":
        return '<div class="legend">' + "".join(
            f'<span><i class="{html.escape(t.strip())}"></i>{inline(label.strip())}</span>'
            for label,t in pipe_rows(block, 2, ["label", "tone"])) + "</div>"
    if k == "bars":
        rows = pipe_rows(block, 3, ["label", "plain value", "split value"])
        data = [{"label": required_text(block, x, "bars label"),
                 "plain": finite_nonnegative(block, y, "plain value"),
                 "split": finite_nonnegative(block, z, "split value")}
                for x,y,z in rows]
        if not any(row["plain"] or row["split"] for row in data):
            fail(block.path, block.line, "bars needs at least one positive value")
        return f'<div class="bars" data-bars="{html.escape(json.dumps(data, separators=(",",":")), quote=True)}"></div>'
    if k == "single-bars":
        rows = pipe_rows(block, 3, ["label", "seconds", "detail"])
        data = [(required_text(block, label, "single-bars label"),
                 finite_nonnegative(block, seconds, "single-bars seconds"),
                 required_text(block, detail, "single-bars detail"))
                for label, seconds, detail in rows]
        maximum = max(value for _, value, _ in data)
        if maximum == 0:
            fail(block.path, block.line, "single-bars needs at least one positive value")
        return '<div class="bars">' + "".join(
            f'<div class="barrow singlebar"><b>{inline(label)}</b><div class="barwrap"><div class="bar s" style="width:{value/maximum*100:.3f}%"></div></div><div class="barwrap singlebar-detail"><span class="barlabel">{value:.1f}s · {inline(detail)}</span></div></div>'
            for label, value, detail in data) + "</div>"
    if k in ("section-map", "sizes", "binary-trace", "paths"):
        try: value = json.loads(b)
        except json.JSONDecodeError as e: fail(block.path, block.line + e.lineno, f"invalid JSON in {k}: {e.msg}")
        validate_json_block(block, value)
        return f'<div class="{k}" data-{k}="{html.escape(b, quote=True)}"></div>'
    fail(block.path, block.line, f"renderer missing directive {k!r}")

def required_text(block, value, label):
    if not isinstance(value, str) or not value.strip():
        fail(block.path, block.line, f"{label} must not be empty")
    return value

def finite_nonnegative(block, value, label):
    try:
        number = float(value)
    except (TypeError, ValueError):
        fail(block.path, block.line, f"{label} must be numeric")
    if not math.isfinite(number) or number < 0:
        fail(block.path, block.line, f"{label} must be finite and non-negative")
    return number

def validate_json_block(block, value):
    """Schemas are intentionally small so malformed interactive data is loud."""
    if block.kind == "section-map":
        if not isinstance(value, list) or not value:
            fail(block.path, block.line, "section-map must contain a non-empty entry list")
        names = set()
        for row in value:
            if not isinstance(row, dict) or set(row) - {"name", "equal", "detail", "status"}:
                fail(block.path, block.line, "section-map entries have unsupported fields")
            if not isinstance(row.get("equal"), bool):
                fail(block.path, block.line, "section-map equal must be a boolean")
            name = required_text(block, row.get("name"), "section-map name")
            required_text(block, row.get("detail"), "section-map detail")
            if "status" in row:
                required_text(block, row["status"], "section-map status")
            if name in names: fail(block.path, block.line, f"duplicate section-map name {name!r}")
            names.add(name)
    elif block.kind == "sizes":
        allowed = {"scale", "metrics", "unit", "plainLabel", "splitLabel", "allLabel", "controls"}
        if not isinstance(value, dict) or set(value) - allowed:
            fail(block.path, block.line, "sizes has unsupported fields")
        if not isinstance(value.get("scale"), (int, float)) or isinstance(value.get("scale"), bool) or not math.isfinite(value["scale"]) or value["scale"] <= 0:
            fail(block.path, block.line, "sizes scale must be a positive finite number")
        if not isinstance(value.get("metrics"), list) or not value["metrics"]:
            fail(block.path, block.line, "sizes needs a non-empty metrics array")
        for label in ("unit", "plainLabel", "splitLabel", "allLabel"):
            if label in value: required_text(block, value[label], f"sizes {label}")
        names = set()
        for row in value["metrics"]:
            if not isinstance(row, dict) or set(row) - {"name", "label", "plain", "split", "note"}:
                fail(block.path, block.line, "sizes metric has unsupported fields")
            name = required_text(block, row.get("name"), "sizes metric name")
            if name in names: fail(block.path, block.line, f"duplicate sizes metric {name!r}")
            names.add(name)
            if "label" in row: required_text(block, row["label"], "sizes metric label")
            required_text(block, row.get("note"), "sizes metric note")
            for field in ("plain", "split"):
                metric = row.get(field)
                if not isinstance(metric, (int, float)) or isinstance(metric, bool) or not math.isfinite(metric) or metric < 0:
                    fail(block.path, block.line, f"sizes metric {field} must be finite and non-negative")
            if row["plain"] > value["scale"] or row["split"] > value["scale"]:
                fail(block.path, block.line, "sizes values may not exceed the declared shared scale")
        if "controls" in value:
            controls = value["controls"]
            if not isinstance(controls, list) or not all(isinstance(x, str) for x in controls):
                fail(block.path, block.line, "sizes controls must be a list of metric names")
            if len(controls) != len(set(controls)) or not all(x in names for x in controls):
                fail(block.path, block.line, "sizes controls must be unique metric names")
    elif block.kind == "binary-trace":
        if not isinstance(value, dict) or not value:
            fail(block.path, block.line, "binary-trace must be a non-empty object")
        for key, row in value.items():
            if not isinstance(key, str) or not key.strip() or not isinstance(row, dict) or set(row) - {"source", "plain", "split", "note", "label", "plainLabel", "splitLabel"}:
                fail(block.path, block.line, "binary-trace entry has unsupported fields")
            for field in ("source", "plain", "split", "note"):
                required_text(block, row.get(field), f"binary-trace {field}")
            for field in ("label", "plainLabel", "splitLabel"):
                if field in row: required_text(block, row[field], f"binary-trace {field}")
    elif block.kind == "paths":
        if not isinstance(value, dict) or not value:
            fail(block.path, block.line, "paths must be a non-empty object of button labels to explanations")
        for key, description in value.items():
            required_text(block, key, "paths label")
            required_text(block, description, "paths explanation")

def parse_snippets(path):
    if not path.exists(): fail(path, 1, "missing shared snippets file")
    text, out = path.read_text(), {}
    pattern = re.compile(r"^::: snippet ([a-z0-9_-]+) \| (.+?) \| (.+?)\n```[^\n]*\n(.*?)^```\n^:::$", re.M | re.S)
    for match in pattern.finditer(text):
        key, title, desc, code = match.groups()
        if key in out: fail(path, text[:match.start()].count("\n") + 1, f"duplicate snippet {key!r}")
        out[key] = (title, code.rstrip("\n"), desc)
    if not out: fail(path, 1, "no snippets found")
    return out

def read_deck(source):
    source = Path(source)
    manifest = source / "manifest.txt"
    if not manifest.exists(): fail(manifest, 1, "missing explicit manifest.txt")
    names = [x.strip() for x in manifest.read_text().splitlines() if x.strip() and not x.startswith("#")]
    if len(names) != len(set(names)): fail(manifest, 1, "manifest contains duplicate slide names")
    if not names: fail(manifest, 1, "manifest has no slides")
    slides = []
    for name in names:
        if "/" in name or not name.endswith(".md"): fail(manifest, 1, f"invalid slide path {name!r}")
        path = source / name
        if not path.exists(): fail(manifest, 1, f"missing slide {name!r}")
        slide = parse_markdown(path, path.read_text())
        for block in slide.blocks: block.path = path
        slides.append(slide)
    return slides

def asset(path):
    if not path.exists():
        fail(path, 1, "required build asset is missing")
    return path.read_text()

def render(source):
    """Validate and render all sources, without changing the filesystem."""
    source = Path(source).resolve()
    snippets = parse_snippets(source.parent / "snippets.md")
    slides = read_deck(source)
    rendered = []
    for slide in slides:
        contents = "".join(render_block(block, snippets) for block in slide.blocks)
        rendered.append({"chapter": slide.meta["chapter"], "notes": slide.meta["notes"],
                         # These are consumed with textContent by runtime.js.
                         "eyebrow": slide.meta.get("eyebrow",""),
                         "chapterlabel": slide.meta.get("chapter-label",""),
                         "layout": slide.meta.get("layout",""),
                         "content": contents})
    data = json.dumps({"slides": rendered, "snippets": snippets}, ensure_ascii=False,
                      separators=(",", ":"), sort_keys=True).replace("<", "\\u003c")
    template_path, style_path, runtime_path = HERE / "template.html", HERE / "theme.css", HERE / "runtime.js"
    template, style, runtime = asset(template_path), asset(style_path), asset(runtime_path)
    logo = "data:image/svg+xml;base64," + base64.b64encode(asset(HERE / "engflow.svg").encode("utf-8")).decode("ascii")
    for marker, replacement in (("/*__STYLE__*/", style), ("/*__RUNTIME__*/", runtime), ("/*__DECK__*/", data), ("/*__LOGO__*/", logo)):
        if marker not in template:
            fail(template_path, 1, f"missing template marker {marker}")
        template = template.replace(marker, replacement)
    return template, len(slides)

def atomic_write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as handle:
            handle.write(content)
        os.replace(temporary, path)
    except Exception:
        try: os.unlink(temporary)
        except FileNotFoundError: pass
        raise

def build(source, output):
    document, count = render(source)
    atomic_write(output, document)
    return count

def watch_inputs(source, output=None):
    """Return only actual build inputs, never the generated output."""
    source = Path(source).resolve()
    manifest = source / "manifest.txt"
    if not manifest.exists(): fail(manifest, 1, "missing explicit manifest.txt")
    names = [x.strip() for x in manifest.read_text().splitlines() if x.strip() and not x.startswith("#")]
    paths = [manifest, source.parent / "snippets.md", HERE / "template.html", HERE / "theme.css", HERE / "runtime.js", HERE / "engflow.svg"]
    for name in names:
        if "/" not in name and name.endswith(".md"):
            paths.append(source / name)
    excluded = Path(output).resolve() if output else None
    return tuple(sorted({path.resolve() for path in paths if path.resolve() != excluded}, key=str))

def watch_stamp(source, output=None):
    paths = watch_inputs(source, output)
    return tuple((str(path), path.stat().st_mtime_ns if path.exists() else None) for path in paths)

def main(argv=None):
    p = argparse.ArgumentParser(description="Build the offline Markdown-authored slide deck.")
    p.add_argument("--source", default=str(HERE / "slides"))
    p.add_argument("--output", default=str(ROOT / "slides.html"))
    p.add_argument("--check", action="store_true", help="validate sources without writing")
    p.add_argument("--watch", action="store_true", help="rebuild after source changes")
    args = p.parse_args(argv)
    def once():
        if args.check:
            _, count = render(args.source)
        else:
            count = build(args.source, args.output)
        print(f"presentation: {'validated' if args.check else 'built'} {count} slides")
    try:
        once()
        if args.watch:
            stamps = watch_stamp(args.source, args.output)
            while True:
                try:
                    now = watch_stamp(args.source, args.output)
                except (SourceError, OSError) as e:
                    print(f"presentation: {e}", file=sys.stderr)
                    time.sleep(.5)
                    continue
                if now != stamps:
                    stamps = now
                    try: once()
                    except (SourceError, OSError) as e: print(f"presentation: {e}", file=sys.stderr)
                time.sleep(.5)
    except SourceError as e:
        print(f"presentation: {e}", file=sys.stderr); return 2
    except OSError as e:
        print(f"presentation: {e}", file=sys.stderr); return 2
    return 0

if __name__ == "__main__":
    raise SystemExit(main())