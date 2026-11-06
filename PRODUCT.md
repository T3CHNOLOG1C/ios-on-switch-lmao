# Product

<!-- impeccable:product-schema 1 -->

Written without an interview: every fact here comes from README.md, AGENTS.md and docs/. Lines marked (inferred) are
readings of those files that nobody has confirmed yet.

## Platform

web

## Users

- Switch OLED owners who already boot hekate and want iPadOS on their console. They read the docs at a desk with the
  console and a Mac next to them, one step at a time, and nobody answers their questions anywhere else.
- People who saw the project and want to know how iPadOS runs on a Tegra X1+ at all. (inferred)
- Contributors who build the kernel and drivers from source and send pull requests.

## Product Purpose

The project boots Apple's iPadOS 26 and 27 natively on a Nintendo Switch OLED: XNU built from Apple's published source,
drivers for the Switch's chips, and Apple's own iPadOS above it, downloaded by the image builder on a Mac. The docs site
gets an owner from an empty SD-card partition to the Home Screen, says plainly what works and what doesn't, and explains
how it works.

## Positioning

iPadOS itself runs on the console's hardware, with no emulator: Apple's kernel and userland, and the project's own
drivers, arm64e translation and Metal-to-Vulkan path between them.

## Operating Context

- Started from hekate's **More Configs** menu, next to Android and Linux, with its own menu icon
  (`bootloader/res/icon_ios.bmp`).
- Installed by an image builder that runs on macOS (`bazelisk run //:image`) and a hekate payload (`install-ios.bin`).
- Docs are a Mintlify site built from `docs/`, served at ios-on-switch-lmao.brunostjohn.com.

## Capabilities and Constraints

- Switch OLED only. No other model is supported or planned.
- No support: issues are disabled, pull requests are the only contributions, with no review timeline.
- Apple services (activation, Apple Account, iCloud, App Store, FairPlay) are out of scope.

## Brand Commitments

- The name is `ios-on-switch-lmao`, lowercase. Code and assets never use the old internal name.
- Voice: plain and direct, with dry humour in the README. No flowery language, no lists of what something isn't.
- Apple's logo artwork and Nintendo's marks are never the project's identity; the README carries the trademark notice.
- The Apple boot logo on screen while booting is Apple's and stays pixel-identical from hekate to backboardd. It is not
  part of the project's brand.

## Evidence on Hand

- The facts in docs/*.mdx: supported hardware, the install steps, what works and what doesn't.
- No photos or screenshots of the Switch running iPadOS exist yet (`TODO(photo)` in docs/index.mdx and README.md). Don't
  fabricate one.

## Product Principles

- What doesn't work is listed as plainly as what does.
- The reader is on their own, so every step has to stand without help.
- The port's engineering rules (never slower, native pixels, no polling) are part of what the docs explain.
