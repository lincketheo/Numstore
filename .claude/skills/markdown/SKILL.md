---
name: plain-text-markdown
description: Write or edit Markdown (READMEs, docs, notes) in a plain-text-first style: setext headings, 80-column wrap, aligned tables, labeled code blocks, minimal bold.
---

Plain-text-first Markdown
=========================

Use this whenever you write or edit a Markdown file: READMEs, docs, guides,
notes, changelogs.

The one principle: the reader is in a plain text editor with no renderer. The
file must read well as raw text first, and be valid Markdown second. Every rule
below follows from that. When in doubt, look at the raw file and ask whether it
reads cleanly without rendering.

Use a minimal set of Markdown: setext headings, paragraphs, `-` or `*` lists,
numbered lists, `>` quotes, fenced code blocks, inline code, links and aligned
tables. Nothing else unless there is a real reason.

Headings
--------

Use setext headings. The underline is exactly as long as the title.

```markdown
Numstore
========

Core ideas
----------
```

- `=====` for the document title and for top-level parts of a long document.
- `-----` for sections.
- `###` and deeper are allowed but discouraged. Before reaching for a third
  level, rethink: the section can often be split, flattened, or the sub-point
  turned into a short paragraph with a lead-in sentence, a list, or a `>` note.
  Long reference documents (one subsection per command, say) are where deeper
  levels earn their place.
- Sentence case for titles. Short titles.
- One blank line before and after every heading.

Paragraphs and wrapping
-----------------------

- Hard wrap prose at 80 columns. Break at word boundaries; avoid splitting
  inline code or a link across lines.
- Exceptions that may run past 80: code block contents, table rows, headings,
  and a single long URL or inline code span that can't be broken.
- One blank line between paragraphs. No trailing whitespace, no two-space hard
  line breaks, no `<br>`.
- Short paragraphs. Lead with the point.

Lists
-----

- Use `-` or `*` for bullets; stay consistent within a list. `1.` `2.` `3.`
  for ordered steps (write the real numbers, not all `1.`).
- Wrapped continuation lines are indented to line up with the text of the item,
  as in this list.
- Nested lists are indented 2 spaces under the parent's text. Keep nesting to
  one level.
- Put a blank line before a list. Separate items with blank lines only when
  items are multi-sentence paragraphs.
- A bold lead-in is acceptable in a feature-style list (`- **Single file.**
  ...`), but don't make every list look like that.

Emphasis
--------

Minimize it. Raw `**` and `__` clutter plain text, and bold renders heavily.

- Use bold rarely: a tagline, or the one word that must not be missed.
- Use `_underscores_` sparingly to introduce a term the first time.
- Never bold whole sentences or whole paragraphs. Never combine bold and code.
- If something needs drawing attention to, prefer a `>` note or better wording.

Code
----

- Always label fenced code blocks with a language: `python`, `sh`, `c`,
  `toml`, `yaml`, `make`, `json`, `markdown`. Use `text` for ASCII diagrams,
  grammars, file trees and program output.
- Use triple backticks, never indented code blocks.
- Blank line before and after every code block.
- Use inline code for identifiers, commands, file paths, flags and literal
  values: `make test`, `include/numstore.h`, `--tb=short`.
- ASCII diagrams are encouraged when they explain layout or before/after
  states; put them in a `text` block so alignment survives.
- REPL sessions use `>>>` prompts and show real output.

Tables
------

Only use a table for genuinely tabular data (a command and what it does, a type
and its size). Otherwise use a list.

Align the table before you write it: work out each column's widest cell, then
pad every cell to that width so the pipes line up in plain text.

```markdown
| Target         | What it does                       |
| -------------- | ---------------------------------- |
| `make debug`   | debug build into `build/debug`     |
| `make release` | release build into `build/release` |
```

- One space of padding inside each pipe; the dash row fills the column width.
- No alignment colons unless a numeric column truly needs right alignment.
- Keep cells short. If a cell needs a sentence, the table is the wrong shape.
- Tables may exceed 80 columns, but prefer shortening cells to wide tables.

Quotes and notes
----------------

`>` quotes are fine, including nested `>>`. Use them for notes, warnings and
asides instead of bold or deep headings:

```markdown
> Note: `f128` needs a platform where `long double` is 16 bytes.
```

Links
-----

- Inline links with readable text: `[C Quick Start](#c-quick-start)`,
  `[LICENSE](LICENSE)`. Use relative paths for files in the repo.
- Anchors are the lowercased heading with spaces as `-` and punctuation
  dropped. Check every anchor matches a real heading.
- No HTML, images, badges, footnotes, task lists or emoji unless asked.

Table of contents
-----------------

Add one when the document gets long: roughly more than six sections, or longer
than a few screens. Put it near the top, after the title and any short intro,
as its own `=====` heading:

```markdown
Table of Contents
=================

* [What is Numstore?](#what-is-numstore)
  * [How is it different?](#how-is-it-different)
* [Getting started](#getting-started)
* [A tour of the language](#a-tour-of-the-language)
  * [Variable operations](#variable-operations)
  * [Array operations](#array-operations)
```

Use `*` bullets, nest 2 spaces per level, list every heading the reader would
jump to, and keep it in document order.

Before finishing
----------------

Check the raw file:

1. Prose lines are at most 80 columns (code blocks, tables, headings and
   unbreakable URLs excepted). A quick check:

   ```sh
   awk 'length > 80 && !/^ *\|/ { print FILENAME ":" FNR ": " length }' FILE.md
   ```

   Then confirm each hit is inside a code block or is an allowed exception.
2. Every table's pipes line up column by column.
3. Every code fence has a language label.
4. Heading underlines match the title length; `###` and deeper only where
   they earn it.
5. Every `#anchor` link points at a real heading.
6. Bold appears only a handful of times in the whole document.
7. Read the file top to bottom as plain text. If anything looks noisy or
   cluttered without rendering, simplify it.
