---
name: ios-on-switch-lmao
description: The docs site and the project mark, set as the port's service manual.
colors:
  paper: "#FFFFFF"
  paper-2: "#F5F5F2"
  ink: "#16181B"
  ink-2: "#4A4F57"
  body: "#33373D"
  rule: "#DCDDD9"
  kapton-amber: "#F0A21E"
  amber-ink: "#9A5B00"
  dark-paper: "#121315"
  dark-paper-2: "#1A1C1F"
  dark-ink: "#ECEDEA"
  dark-ink-2: "#A3A8AF"
  dark-body: "#C9CCD1"
  dark-rule: "#2B2D31"
typography:
  title:
    fontFamily: "Barlow Semi Condensed, Barlow, sans-serif"
    fontSize: "2.75rem"
    fontWeight: 600
    lineHeight: 1.05
    letterSpacing: "-0.015em"
  heading:
    fontFamily: "Barlow Semi Condensed, Barlow, sans-serif"
    fontSize: "1.75rem"
    fontWeight: 600
    letterSpacing: "-0.01em"
  subheading:
    fontFamily: "Barlow Semi Condensed, Barlow, sans-serif"
    fontSize: "1.3rem"
    fontWeight: 600
  body:
    fontFamily: "Barlow, sans-serif"
    fontSize: "17px"
    fontWeight: 400
    lineHeight: 1.65
rounded:
  plate: "2px"
  tile: "22.4%"
components:
  step-balloon:
    backgroundColor: "{colors.kapton-amber}"
    textColor: "{colors.ink}"
    typography: "{typography.subheading}"
    size: "2rem"
  nav-tab-active:
    backgroundColor: "{colors.kapton-amber}"
    textColor: "{colors.ink}"
    rounded: "{rounded.plate}"
  warning-plate:
    backgroundColor: "{colors.paper}"
    textColor: "{colors.ink}"
    rounded: "{rounded.plate}"
---

<!-- rumdl-disable MD025 MD026 -->

# Design System: ios-on-switch-lmao

## Overview

**Creative North Star:** "The service manual"

The docs read like the console's own repair guide: numbered callout balloons down the install steps, parts lists for
tables, a framed warning plate, and a running head with a strip of Kapton tape under it. Kapton is the amber tape used
in modchip installs, which is how a Switch OLED gets to hekate in the first place.

The frame carries the world and the reading column stays calm. Mintlify draws the navigation, search and components;
docs.json sets the fonts and colours, and docs/style.css restyles the rest.

**Key Characteristics:**

- Paper and ink with one amber, used as a fill and never as text on paper.
- Condensed titles and labels over a plain reading face.
- Square 2px corners and 1px rules. No shadows.

## Colors

Two inks on paper, and Kapton amber for the few things that mark the reader's place.

### Primary

- **Kapton amber** (#F0A21E): step balloons, the active sidebar tab, the strip under the navbar, the warning plate's
  header band and the tile behind the mark, plus text selection and focus rings. Ink sits on it at 8.4:1.
- **Amber ink** (#9A5B00): Mintlify's primary colour in light mode, for the few accents the stylesheet doesn't reach.
  5.4:1 on paper. Dark mode uses Kapton amber itself.

### Neutral

- **Paper** (#FFFFFF, dark #121315): the page.
- **Paper 2** (#F5F5F2, dark #1A1C1F): code blocks, inline code, callouts other than warnings, stack rows, hovers.
- **Ink** (#16181B, dark #ECEDEA): titles, headings, strong text, link text, code, rules that open a section.
- **Body** (#33373D, dark #C9CCD1): running text.
- **Ink 2** (#4A4F57, dark #A3A8AF): standfirsts, the contents list's descriptions and leaders, link underlines, list
  markers, search and footer.
- **Rule** (#DCDDD9, dark #2B2D31): hairlines between rows and around plates.

### Named Rules

**The Tape Rule.** Amber marks where the reader is or what to watch: the current page, the current step, the
warning. It never colours text, links or status marks, and code has no syntax hues: nothing else gets a hue.

**The Mark-Not-Hue Rule.** A status is a drawn mark beside a word: a filled ink square for what works, an open ink
square for what doesn't yet.

## Typography

- **Display Font:** Barlow Semi Condensed 600, self-hosted in docs/fonts.
- **Body Font:** Barlow 400, with 400 italic and 600, self-hosted in docs/fonts (OFL).

**Character:** a DIN-like grotesk, condensed for the labels of a technical drawing and regular width for reading.

### Hierarchy

- **Title** (600, 2.75rem, 1.05): the page title, over a 1px ink rule with the standfirst.
- **Heading** (600, 1.75rem): h2, with a 1px rule above it and more space above than below.
- **Subheading** (600, 1.3rem): h3, step titles, the contents list's titles.
- **Body** (400, 17px, 1.65): running text, about 75 characters to the line in Mintlify's column.
- **Label** (600, 0.95–1.1rem): sidebar group names, table headers, stack row names.

## Layout

Mintlify's sequoia theme: running head, sidebar, a 40.5rem column and the page's contents on the right, separated by
hairlines. Below 640px the contents list and the stack drop to one column.

## Elevation & Depth

Flat. Depth comes from rules and plates: a 1px frame, paper 2 behind it, or a 5px inset amber band along a plate's
top. No drop shadows anywhere.

## Shapes

2px corners on plates, code, inputs, pagination and the active tab; full circles only on step balloons. The mark sits
on a rounded tile (radius 224 of 1000), an iOS Home Screen icon's shape because it stands for an iPadOS install. In
hekate's menu it's a full-bleed square, because Nyx draws its own button frame.

## Components

### Navigation

- The sidebar's active page is a filled amber tab with ink text; other pages get paper 2 on hover. Group names are
  labels in ink.
- A 3px amber strip runs under the navbar. The section eyebrow above the title is hidden.

### Step balloons

Mintlify's Steps, restyled: a 2rem amber circle with the step number in ink at 600 with tabular figures, on a 1px leader
line down to the next step.

### Warning plate

A 1px ink frame on paper, with a 5px amber header band along the top, ink text at 1rem.

### Contents list

The index's way in: a 1px ink rule, then one row per page with the title in the label face, a dotted leader and its
one-line description, hairlines between rows, paper 2 on hover.

### Parts list

Installation's "What you need": ruled rows under a 1px ink rule, each with its item number in the label face.

### Stack

How it works opens with the boot chain as stacked plates: the Tegra X1+ at the bottom on a 6px ink base, iPadOS at the
top framed in ink on paper. The DOM lists them in boot order.

### The mark

The Switch pulled apart into its three pieces, the middle one an iPad screen with its home indicator, in ink on a
Kapton amber tile. `.github/logo.svg` is the one source: a square viewBox at 0, one `<rect>` for the tile and one
`<path>` for the mark, which boot/logo rasterises into hekate's `icon_ios.bmp`.

## Do's and Don'ts

### Do:

- **Do** keep amber to the current page, the current step, the warning and the mark.
- **Do** set links in ink with a 1px ink 2 underline, offset 0.2em.
- **Do** put a drawn mark next to a status word.
- **Do** keep `.github/logo.svg` to one `<rect>` and one `<path>` with no transform, or boot/logo can't read it.

### Don't:

- **Don't** set amber text on paper: it's 2.1:1.
- **Don't** add drop shadows or radii above 2px on plates.
- **Don't** use Apple's or Nintendo's marks as the project's. The Apple boot logo belongs to the boot path, not this
  system.
