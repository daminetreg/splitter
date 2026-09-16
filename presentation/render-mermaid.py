#!/usr/bin/env python3
"""Pre-render every `::: mermaid` block of the deck to presentation/diagrams/<sha1>.svg.

The deck is a single offline HTML file, so the diagrams are drawn once here -- headless
Chrome, mermaid from a CDN, the deck's dark theme -- and build.py inlines the SVG. A block
whose text changes gets a new hash and a new file; stale files are left for git to show.
Run after editing a diagram; build.py says which hash it is missing.
"""
import hashlib, html, re, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "diagrams"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

def blocks(text):
    found = []
    for kind, body in re.findall(r"^::: (mermaid|mermaid-columns)\n(.*?)^:::$", text, re.S | re.M):
        found += re.split(r"^---$", body, flags=re.M) if kind == "mermaid-columns" else [body]
    return found

def digest(source):
    return hashlib.sha1(source.strip().encode()).hexdigest()[:12]

# The deck's semantic colours as mermaid classes, appended to every flowchart so a slide can
# write `class launcher,worker split` -- the same names as the {split}/{violet}/{accent}/{red}
# text tones, plus `warn` and `muted`.
DECK_CLASSES = """
    classDef split fill:#12211f,stroke:#55ddc3,stroke-width:2px,color:#e8eeea
    classDef accent fill:#12211f,stroke:#55ddc3,stroke-width:2px,color:#e8eeea
    classDef violet fill:#1a1830,stroke:#b4a0ff,stroke-width:2px,color:#e8eeea
    classDef warn fill:#2a2116,stroke:#ffbf7c,stroke-width:2px,color:#e8eeea
    classDef red fill:#2b1916,stroke:#ff826d,stroke-width:2px,color:#e8eeea
    classDef muted fill:#172321,stroke:#31433f,stroke-width:1px,color:#9aaba5
"""

def with_deck_classes(source):
    head = source.lstrip().split("\n", 1)[0]
    if head.startswith(("flowchart", "graph", "classDiagram")):
        return source.rstrip() + "\n" + DECK_CLASSES
    return source

def render(source):
    source = with_deck_classes(source)
    page = f"""<!doctype html><html><body>
<pre class="mermaid">{html.escape(source)}</pre>
<script type="module">
import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
mermaid.initialize({{ startOnLoad: false, theme: 'dark', fontFamily: 'system-ui, sans-serif',
  themeVariables: {{ background: 'transparent', primaryColor: '#172321', primaryBorderColor: '#55ddc3',
    primaryTextColor: '#e8eeea', lineColor: '#9aaba5', secondaryColor: '#1f2d2a', tertiaryColor: '#111c19',
    fontSize: '18px', clusterBkg: '#111c19', clusterBorder: '#3c5558', titleColor: '#9aaba5', edgeLabelBackground: '#101817' }} }});
try {{ await mermaid.run(); document.title = 'OK'; }} catch (e) {{ document.title = 'ERR ' + e.message; }}
</script></body></html>"""
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False) as f:
        f.write(page); path = f.name
    dom = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--virtual-time-budget=15000",
                          "--dump-dom", "file://" + path], capture_output=True, text=True).stdout
    title = re.search(r"<title>(.*?)</title>", dom)
    if not title or title.group(1) != "OK":
        raise RuntimeError(f"mermaid failed: {title.group(1) if title else 'no render'}\n{source}")
    svg = re.search(r"<svg[^>]*id=\"mermaid[^>]*>.*?</svg>", dom, re.S)
    if not svg:
        raise RuntimeError("no <svg> came back")
    return svg.group(0)

def render_missing(slides_dir=None):
    """Render every diagram of the manifest that has no SVG yet; returns how many. build.py
    calls this before each build, so `--watch` picks up an edited diagram on its own."""
    slides_dir = Path(slides_dir) if slides_dir else HERE / "slides"
    OUT.mkdir(exist_ok=True)
    manifest = (slides_dir / "manifest.txt").read_text().splitlines()
    done = 0
    for name in manifest:
        name = name.strip()
        if not name or name.startswith("#"): continue
        for source in blocks((slides_dir / name).read_text()):
            target = OUT / (digest(source) + ".svg")
            if target.exists(): continue
            if not Path(CHROME).exists():
                raise RuntimeError(f"{name}: diagram {target.name} needs rendering and Chrome is not at {CHROME}")
            target.write_text(render(source))
            print(f"{name}: rendered {target.name}", flush=True)
            done += 1
    return done

def main():
    print(f"{render_missing()} diagram(s) rendered")

if __name__ == "__main__":
    main()
