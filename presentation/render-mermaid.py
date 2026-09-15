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
    return re.findall(r"^::: mermaid\n(.*?)^:::$", text, re.S | re.M)

def digest(source):
    return hashlib.sha1(source.strip().encode()).hexdigest()[:12]

def render(source):
    page = f"""<!doctype html><html><body>
<pre class="mermaid">{html.escape(source)}</pre>
<script type="module">
import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
mermaid.initialize({{ startOnLoad: false, theme: 'dark', fontFamily: 'system-ui, sans-serif',
  themeVariables: {{ background: 'transparent', primaryColor: '#172321', primaryBorderColor: '#55ddc3',
    primaryTextColor: '#e8eeea', lineColor: '#9aaba5', secondaryColor: '#1f2d2a', tertiaryColor: '#111c19',
    fontSize: '18px' }} }});
try {{ await mermaid.run(); document.title = 'OK'; }} catch (e) {{ document.title = 'ERR ' + e.message; }}
</script></body></html>"""
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False) as f:
        f.write(page); path = f.name
    dom = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--virtual-time-budget=15000",
                          "--dump-dom", "file://" + path], capture_output=True, text=True).stdout
    title = re.search(r"<title>(.*?)</title>", dom)
    if not title or title.group(1) != "OK":
        sys.exit(f"mermaid failed: {title.group(1) if title else 'no render'}\n{source}")
    svg = re.search(r"<svg[^>]*id=\"mermaid[^>]*>.*?</svg>", dom, re.S)
    if not svg:
        sys.exit("no <svg> came back")
    return svg.group(0)

def main():
    OUT.mkdir(exist_ok=True)
    manifest = (HERE / "slides" / "manifest.txt").read_text().splitlines()
    done = 0
    for name in manifest:
        name = name.strip()
        if not name or name.startswith("#"): continue
        for source in blocks((HERE / "slides" / name).read_text()):
            target = OUT / (digest(source) + ".svg")
            if target.exists(): continue
            target.write_text(render(source))
            print(f"{name}: rendered {target.name}")
            done += 1
    print(f"{done} diagram(s) rendered")

if __name__ == "__main__":
    main()
