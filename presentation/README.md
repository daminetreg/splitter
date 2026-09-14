# Markdown slide authoring

Run `python3 presentation/build.py` to generate the offline `slides.html`.
`--check` renders and validates every input without changing the deck; `--watch`
rebuilds after an input changes. It watches exactly the manifest, listed
Markdown slides, shared snippets, and `template.html`, `theme.css`, and
`runtime.js`; generated output is excluded even when it is placed nearby.
`--source DIR` and `--output PATH` make temporary decks or CI destinations
explicit. Builds are atomic, so an authoring error leaves the last successful
deck untouched. Errors name the Markdown file and source line.

`slides/manifest.txt` is the sole ordering mechanism. Each listed file is one
visible slide, and all 30 source files are deliberately separate. A slide has
front matter:

```markdown
---
chapter: Architecture
notes: Speaker notes appear through NOTES.
eyebrow: Optional small label
---
## A heading with a  
deliberate break and {accent}semantic colour{/accent}.
```

Ordinary paragraphs are Markdown prose. Use `**strong**`, two trailing spaces
for a deliberate line break, and `{accent}`, `{violet}`, `{split}`, or `{red}`
with the matching closing tag for the existing semantic colours. Fenced code
is literal, safely escaped, and receives the deck code treatment. Raw HTML is
intentionally rejected.

The small layout vocabulary is shown by the migrated slides:

* `::: flow` has `- label | tone` rows (`tone` may be blank, `teal`, `violet`,
  or `split`); an optional third field is a shared popup reference. See
  `02-proposition.md` and `08-mirror.md`.
* `::: fission` has `- label | tone` rows: the first row is the whole, every
  later row one of the pieces it breaks into, rendered one → many. See
  `02-proposition.md`.
* `::: cards 2`, `::: cards 3`, and `::: metrics 3` use
  `- title-or-value | tone | description`; cards accept an optional fourth
  popup reference. See `06-classification.md` and `21-benchmark-actions.md`.
* `tree`, `rationale`, `tiny`, `callout`, and `flag` contain normal prose;
  see `03-input.md` and `15-controlled-comparison.md`.
* `legend` has `- label | class`, and `bars` has
  `- label | plain-seconds | splitter-seconds`; bars derive their scale from
  their own declared set (`20-benchmark-wall.md`).
  `single-bars` is the one-series remote chart with
  `- label | seconds | detail` rows (`22-benchmark-remote.md`).
* `code-columns` holds exactly two normal fenced code blocks separated by
  `---`, preserving side-by-side source comparisons (`10-context.md`).
* `semantic left label | right label` holds two fenced `diff` blocks separated
  by `---`. Prefix equal lines with `=` and metadata differences with `+` to
  retain the coloured symbol comparison (`16-semantic-diff.md`).
* `logic` has one `- left node | decision | right node` row. `map` has
  `- name | detail | tone` rows. `flag` is the bordered comparison line.
  These complete the small, fixed vocabulary; unknown directives fail rather
  than becoming unrendered HTML.
  Use `\n` for a node line break, and `flow merge=N` to group N sibling inputs
  before the first arrow, as in the relocatable-link slide.
* `section-map`, `sizes`, and `binary-trace` take readable, indented JSON
  structured data for the interactive evidence layouts. `18-paired-sizes.md`
  declares the canonical shared `8880` byte scale explicitly.
  Section-map entries require `name`, boolean `equal`, and `detail`, with
  optional `status`. Size data requires positive finite `scale` and non-empty,
  uniquely named metrics (`name`, `plain`, `split`, `note`); optional
  `unit`, `plainLabel`, `splitLabel`, `allLabel`, `controls`, and
  per-metric `label` only change presentation labels. Binary-trace entries
  require `source`, `plain`, `split`, and `note`, with optional `label`,
  `plainLabel`, and `splitLabel`.
* `paths` is a JSON object whose keys become launcher-path buttons and whose
  values are their explanations; see `04-contract.md`.

Shared popup source belongs in `presentation/snippets.md`, where each
`::: snippet id | title | card description` block contains a normal fenced
code block.
Reference it from a slide with `::: popup id`; missing and duplicate IDs fail
the build. Keep numbers and claims in the relevant slide instead of adding
JavaScript: the renderer is generic and no slide data is hardcoded in it.