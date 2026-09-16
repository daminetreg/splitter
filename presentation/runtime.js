/* Shared behavior only. Slide content and interactive data come from Markdown. */
(() => {
  "use strict";
  const data = JSON.parse(document.querySelector("#deck-data").textContent);
  const root = document.querySelector("#deck");
  const make = (tag, cls, text) => {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text !== undefined) el.textContent = text;
    return el;
  };
  const attrData = (el, name) => JSON.parse(el.getAttribute(`data-${name}`));
  const setSelected = (buttons, selected) => {
    for (const b of buttons) {
      b.classList.toggle("active", b === selected);
      b.setAttribute("aria-pressed", String(b === selected));
    }
  };

  for (const slide of data.slides) {
    const el = make("section", "slide");
    el.dataset.chapter = slide.chapter;
    el.dataset.notes = slide.notes;
    if (slide.layout) el.dataset.layout = slide.layout;
    if (slide.eyebrow) el.append(make("div", "eyebrow", slide.eyebrow));
    if (slide.chapterlabel) el.append(make("div", "chapter", slide.chapterlabel));
    // This fragment is escaped and rendered by the trusted Markdown builder.
    const body = document.createElement("template");
    body.innerHTML = slide.content;
    el.append(body.content, make("span", "num"));
    root.append(el);
  }
  const slides = [...root.querySelectorAll(".slide")];
  const progress = document.querySelector("#progress");
  const notes = document.querySelector("#notes");
  const notesToggle = document.querySelector("#notesToggle");
  const chapters = document.querySelector("#chapters");
  const modal = document.querySelector("#modal");
  const closeButton = document.querySelector("#close");
  const modalTitle = document.querySelector("#modalTitle");
  const modalCode = document.querySelector("#modalCode");
  let current = 0;
  let opener = null;

  slides.forEach((slide, i) => {
    slide.querySelector(".num").textContent = `${String(i + 1).padStart(2, "0")} / ${slides.length}`;
  });
  for (const chapter of new Set(slides.map(s => s.dataset.chapter))) {
    const b = make("button", "", chapter);
    b.dataset.chapter = chapter;
    b.onclick = () => show(slides.findIndex(s => s.dataset.chapter === chapter));
    chapters.append(b);
  }
  function show(n, updateHash = true) {
    current = Math.min(slides.length - 1, Math.max(0, Number.isFinite(n) ? n : 0));
    slides.forEach((slide, i) => slide.classList.toggle("active", i === current));
    progress.style.width = `${(current + 1) / slides.length * 100}%`;
    notes.textContent = slides[current].dataset.notes;
    for (const b of chapters.children) b.classList.toggle("on", b.dataset.chapter === slides[current].dataset.chapter);
    if (updateHash) {
      try { history.replaceState(null, "", `#${current + 1}`); }
      catch { location.hash = String(current + 1); }
    }
    window.scrollTo(0, 0);
  }
  notesToggle.onclick = () => {
    notes.classList.toggle("show");
    notesToggle.setAttribute("aria-expanded", String(notes.classList.contains("show")));
  };
  function closeModal() {
    modal.classList.remove("show");
    opener?.focus();
  }
  closeButton.onclick = closeModal;
  modal.onclick = e => { if (e.target === modal) closeModal(); };
  for (const button of root.querySelectorAll("[data-popup]")) {
    button.onclick = () => {
      const source = data.snippets[button.dataset.popup];
      opener = button;
      modalTitle.textContent = source[0];
      modalCode.textContent = source[1];
      highlight(modalCode);
      modal.classList.add("show");
      closeButton.focus();
    };
    button.onkeydown = e => {
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        e.stopPropagation();
        button.click();
      }
    };
  }

  for (const box of root.querySelectorAll("[data-bars]")) {
    const rows = attrData(box, "bars");
    const three = rows.some(row => row.unity !== undefined);
    const max = Math.max(...rows.flatMap(row => [row.plain, row.unity ?? 0, row.split]));
    for (const item of rows) {
      const row = make("div", three ? "barrow three" : "barrow");
      row.append(make("b", "", item.label));
      const series = three ? [[item.plain, "bar"], [item.unity, "bar u"], [item.split, "bar s"]]
                           : [[item.plain, "bar"], [item.split, "bar s"]];
      for (const [value, cls] of series) {
        const track = make("div", "barwrap");
        const bar = make("div", cls);
        bar.style.width = `${max ? value / max * 100 : 0}%`;
        if (!value) bar.style.minWidth = "0";
        track.append(bar);
        row.append(track);
      }
      row.append(make("span", "barlabel", item.caption ?? (three
        ? `${item.plain.toFixed(1)}s / ${item.unity.toFixed(1)}s / ${item.split.toFixed(1)}s`
        : `${item.plain.toFixed(1)}s / ${item.split.toFixed(1)}s`)));
      box.append(row);
    }
  }
  for (const box of root.querySelectorAll("[data-paths]")) {
    const paths = attrData(box, "paths");
    const controls = make("div", "path-controls");
    const explanation = make("div", "pathview");
    for (const [label, text] of Object.entries(paths)) {
      const b = make("button", "pathbtn", label);
      b.onclick = () => {
        setSelected(controls.children, b);
        explanation.textContent = text;
      };
      controls.append(b);
    }
    box.append(controls, explanation);
    controls.firstElementChild.click();
  }
  for (const box of root.querySelectorAll("[data-section-map]")) {
    const sections = attrData(box, "section-map");
    const grid = make("div", "evidence-grid");
    const detail = make("div", "evidence-detail");
    detail.setAttribute("aria-live", "polite");
    box.closest(".slide").dataset.layout = "section-map";
    for (const section of sections) {
      const b = make("button", `section-chip ${section.equal ? "equal" : "diff"}`);
      b.append(make("strong", "", section.name), make("span", "status", section.status ?? (section.equal ? "✓ equal" : "△ differs")));
      b.onclick = () => {
        setSelected(grid.children, b);
        detail.textContent = section.detail;
      };
      grid.append(b);
    }
    grid.onkeydown = e => {
      if (!["ArrowRight", "ArrowLeft", "ArrowUp", "ArrowDown"].includes(e.key)) return;
      const buttons = [...grid.children];
      const pos = buttons.indexOf(document.activeElement);
      if (pos < 0) return;
      e.preventDefault();
      const delta = ["ArrowLeft", "ArrowUp"].includes(e.key) ? -1 : 1;
      buttons[(pos + delta + buttons.length) % buttons.length].focus();
    };
    box.append(grid, detail);
    grid.firstElementChild.click();
  }
  for (const box of root.querySelectorAll("[data-sizes]")) {
    const config = attrData(box, "sizes");
    const controls = make("div", "path-controls");
    const board = make("div", "size-board");
    const unit = config.unit ?? "B";
    const plainLabel = config.plainLabel ?? "plain";
    const splitLabel = config.splitLabel ?? "split";
    function draw(key) {
      const legend = make("div", "legend");
      for (const [name, cls] of [[plainLabel, ""], [splitLabel, "s"]]) {
        const label = make("span");
        label.append(make("i", cls), document.createTextNode(name));
        legend.append(label);
      }
      legend.append(make("span", "", `0–${config.scale.toLocaleString("en-US")} ${unit} shared scale`));
      board.replaceChildren(legend);
      for (const metric of config.metrics.filter(m => key === null || m.name === key)) {
        const row = make("div", "size-row");
        row.append(make("b", "", metric.label ?? metric.name));
        for (const [value, cls, label] of [[metric.plain, "size-fill", plainLabel], [metric.split, "size-fill splitfill", splitLabel]]) {
          const track = make("div", "size-track");
          track.setAttribute("aria-label", `${label}: ${value} ${unit}, scale 0 to ${config.scale}`);
          const fill = make("div", cls);
          fill.style.width = `${value / config.scale * 100}%`;
          track.append(fill);
          row.append(track);
        }
        const values = make("span", "size-value", `plain ${metric.plain} · split ${metric.split} ${unit}`);
        values.append(make("br"), document.createTextNode(metric.note));
        row.append(values);
        board.append(row);
      }
    }
    const choices = [null, ...(config.controls ?? config.metrics.map(m => m.name))];
    for (const key of choices) {
      const metric = config.metrics.find(m => m.name === key);
      const b = make("button", "pathbtn", key === null ? (config.allLabel ?? "all metrics") : (metric.label ?? key));
      b.onclick = () => { setSelected(controls.children, b); draw(key); };
      controls.append(b);
    }
    box.append(controls, board);
    controls.firstElementChild.click();
  }
  for (const box of root.querySelectorAll("[data-binary-trace]")) {
    const traces = attrData(box, "binary-trace");
    const controls = make("div", "path-controls");
    const grid = make("div", "trace-layout");
    const source = make("pre", "code");
    const result = make("pre", "code trace-result");
    const detail = make("div", "evidence-detail");
    source.dataset.language = "cpp";
    detail.setAttribute("aria-live", "polite");
    grid.append(source, result);
    for (const [key, trace] of Object.entries(traces)) {
      const b = make("button", "pathbtn", trace.label ?? key);
      b.onclick = () => {
        setSelected(controls.children, b);
        source.textContent = trace.source;
        highlight(source);
        result.textContent = `${trace.plainLabel ?? "PLAIN"}\n${trace.plain}\n\n${trace.splitLabel ?? "SPLIT"}\n${trace.split}`;
        detail.textContent = trace.note;
      };
      controls.append(b);
    }
    box.append(controls, grid, detail);
    controls.firstElementChild.click();
  }

  // The same offline C++ highlighting used before the Markdown migration.
  function highlight(block) {
    const source = block.textContent;
    if (block.dataset.highlightedSource === source) return;
    block.dataset.highlightedSource = source;
    const keywords = new Set("alignas alignof auto break case catch class constexpr consteval constinit const continue decltype default delete do else enum explicit extern false for friend if inline mutable namespace new noexcept nullptr operator override private protected public register reinterpret_cast return sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typename union using virtual volatile while __attribute__".split(" "));
    const types = new Set("void bool char short int long float double signed unsigned wchar_t size_t string vector numeric iterator T std".split(" "));
    const pattern = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(^[ \t]*#[ \t]*[a-zA-Z_]\w*)|\b(0[xX][\da-fA-F]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?[uUlLfF]*)\b|\b([A-Za-z_]\w*)\b|(::|->|&&|\|\||==|!=|<=|>=|[{}()[\];,+*/%=<>!&|~?:.-])/gm;
    const out = document.createDocumentFragment();
    let end = 0;
    for (const m of source.matchAll(pattern)) {
      out.append(document.createTextNode(source.slice(end, m.index)));
      const [text, comment, string, directive, number, word, operator] = m;
      const kind = comment ? "comment" : string ? "string" : directive ? "directive" : number ? "number" : operator ? "operator" : keywords.has(word) ? "keyword" : types.has(word) ? "type" : word && /^\s*(?:<[^;\n{}]*>)?\s*\(/.test(source.slice(m.index + text.length)) ? "function" : "";
      out.append(kind ? make("span", `tok-${kind}`, text) : document.createTextNode(text));
      end = m.index + text.length;
    }
    out.append(document.createTextNode(source.slice(end)));
    block.replaceChildren(out);
    block.classList.add("syntax");
  }
  root.querySelectorAll('[data-chapter="Architecture"] pre.code, pre[data-language="cpp"]').forEach(highlight);
  const footerLogo = document.querySelector(".engflow-logo");
  if (footerLogo) root.querySelectorAll("img[data-logo]").forEach(img => { img.src = footerLogo.src; });

  document.addEventListener("keydown", e => {
    if (modal.classList.contains("show")) {
      if (e.key === "Escape") { e.preventDefault(); closeModal(); }
      if (e.key === "Tab") { e.preventDefault(); closeButton.focus(); }
      return;
    }
    if (e.target.closest('button,input,textarea,[role="button"],[contenteditable="true"]')) return;
    if (["ArrowRight", "ArrowDown", " ", "PageDown"].includes(e.key)) { e.preventDefault(); show(current + 1); }
    if (["ArrowLeft", "ArrowUp", "PageUp"].includes(e.key)) { e.preventDefault(); show(current - 1); }
    if (e.key === "Home") { e.preventDefault(); show(0); }
    if (e.key === "End") { e.preventDefault(); show(slides.length - 1); }
    if (e.key.toLowerCase() === "n") notesToggle.click();
    if (e.key.toLowerCase() === "p") window.print();
  });
  let touchStart = null;
  document.addEventListener("touchstart", e => {
    touchStart = e.target.closest("pre,button,[role=button],.modal") ? null : e.changedTouches[0].screenX;
  }, {passive: true});
  document.addEventListener("touchend", e => {
    if (touchStart === null || modal.classList.contains("show")) return;
    const delta = e.changedTouches[0].screenX - touchStart;
    if (Math.abs(delta) > 50) show(current + (delta < 0 ? 1 : -1));
    touchStart = null;
  }, {passive: true});
  window.addEventListener("hashchange", () => show(Number(location.hash.slice(1)) - 1, false));
  show(Number(location.hash.slice(1) || "1") - 1, false);
})();