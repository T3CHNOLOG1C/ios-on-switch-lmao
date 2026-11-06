---
version: 1
slug: "docs-docs-json"
primary_target: "docs/docs.json"
related_targets: ["docs/index.mdx","docs/installation.mdx","docs/what-works.mdx","docs/how-it-works.mdx","docs/contributing.mdx",".github/logo.svg"]
---

# Docs site and project mark

Scope: the Mintlify site in docs/ (every page, the index included) and the project mark (.github/logo.svg, the docs
logo and favicon, hekate's menu icon). Mode: Read. Shaped unattended from PRODUCT.md and the repository; nobody
confirmed the direction.

Audience and job: a Switch OLED owner with a modded console and hekate, at a desk with the console, the SD card and a
Mac, following the install one step at a time with nobody to ask. Also readers who want the how, and contributors.
Success: the right page in one click, every step readable at a glance away from the hardware, an honest status list.

Dark mode stays as Mintlify's toggle; light is the default because the reader works at a lit desk. The mark's tile
keeps an iOS Home Screen icon's radius, since it stands for an iPadOS install; hekate's icon is a full-bleed square
because Nyx draws its own button frame.

Constraints: Mintlify draws navigation, search, sidebar and components; the world lives in docs.json theming and one
small stylesheet, whose selectors Mintlify may rename. Prose keeps its meaning and its TODO comments. Boot-logo pixels
are out of scope. Apple's and Nintendo's marks never stand in for the project's.

## Direction contract

THESIS: The docs are the port's service manual: the console's own repair guide, with numbered callout balloons, parts
lists and a warning plate, instead of a SaaS docs template of blue links and icon cards.

OWN-WORLD: Paper white and ink, one Kapton-tape amber (#F0A21E) that owns the balloons, the active tab, the strip under
the running head and the warning plate's header band (and focus and selection), nothing else coloured. Barlow Semi
Condensed for titles and labels, Barlow for reading. Square 2px corners, 1px rules, tabular figures. Status is a drawn
mark and a word, never a hue alone.

STORY: The reader opens the contents, sees what the port is and that it carries no support, goes to Installation, and
works down the balloons with the card in one hand.

FIRST VIEWPORT: Running head with the amber tile, wordmark, search and GitHub over an amber strip; sidebar of section
tabs; the title over a rule, the standfirst, then the warning plate and a contents list with dotted leaders.

FORM: Teardown guide and service manual, number 4 of 7 on the grounded list. seed:1b87d8dac10f8263

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and
every shipping raster carrying its provenance

Raises kept from the declined challengers:
- From the cathode-gauze stack: total palette commitment, ink and amber on paper with no third hue.
- From the cutting-bench rail: state is a mark, not a hue.
- From the tensegrity column: a label sits on a leader line attached to what it names (step connectors).
- From the cyclorama: the stack is ordered from metal up to the Home Screen.
- From the seedbed lobes: amber commits at frame scale (the strip, the active tab), never scattered.

Alternate not built: the game-instruction booklet (number 1 on the list), the most familiar reading of the audience.
