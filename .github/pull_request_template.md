## What

<!-- What the change does, in a sentence or two. -->

## How

<!-- How it does it. Name the files or functions to read first. -->

## Why

<!-- The problem it fixes, or what it makes possible. -->

## Testing and evidence

<!-- What you ran and what happened: tests, a boot on a Switch OLED, log lines, screenshots, before and after numbers. -->

## Risk

<!-- What could break, and on which hardware or setup. If you write "none", say why. -->

## Checklist

- [ ] `bazel test //...`, `bazel test //:format_test //:lint_test` and `bazel run //tools/lint:py_owners` pass
- [ ] I booted this on a Switch OLED, or said under Testing and evidence why the change can't affect the device
- [ ] I've read every line of this diff and can explain it
- [ ] The change does one thing: no unrelated edits, reformatting or drive-by renames
- [ ] Comments and names follow AGENTS.md
- [ ] I understand this may be closed without review, and that there is no timeline for a merge
