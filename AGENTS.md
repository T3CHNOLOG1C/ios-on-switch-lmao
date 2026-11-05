## File Length & Random Tips

Don't write stupidly long files. Around 500 lines of code, you should consider splitting them. Keep helpers away from higher level logic. A human needs to be able to read and maintain this codebase. That means you need to keep it readable to a human. This does not conflict with code comments. Your code should be self explanatory to the largest extent possible.

## Code Comments

Default to writing no new comments.

Add a comment only when it conveys information that a competent
reader cannot reasonably infer from the code itself, such as:

- Non-obvious invariants or constraints
- Why an unconventional implementation is necessary
- Hardware, protocol, or platform-specific quirks
- Subtle concurrency, ordering, or correctness requirements
- Important tradeoffs that future maintainers might otherwise undo

Never write comments that:

- Narrate what the next line or block of code does
- Restate names, types, or obvious control flow
- Divide code into sections with decorative headings
- Describe what you changed, fixed, or added
- Reference the current task or conversation
- Repeat information already expressed by the code

Prefer clear naming and structure over explanatory comments.

Preserve useful existing comments and required API documentation.
Do not add comments or docstrings to unrelated code.

Before finishing, review comments introduced in your diff.
Remove any that do not meet the criteria above.

## Naming

Never write "IOSwitch" or "ioswitch" (any case) in code, file names, logs, paths or identifiers.

- Drivers, classes and log prefixes start with `Switch`: `SwitchSD`, `SwitchCryptex`.
- C identifiers start with `switch_`; macros start with `SWITCH_`.
- Bundle IDs: `com.brunostjohn.switch.<name>`.
- Paths on the iOS filesystem use `Switch`: `/private/var/Switch/...`.
- Bazel module name and host-side tool data: `switch-ios` (`~/Library/Application Support/switch-ios`).
- SD card:
  - `switchroot/ios/`;
  - `bootloader/ini/ios.ini`, hekate entry `iOS`;
  - `bootloader/payloads/install-ios.bin`;
  - `bootloader/res/icon_ios.bmp`.
- Project name `ios-on-switch-lmao`: GitHub `brunostjohn/ios-on-switch-lmao`, docs `ios-on-switch-lmao.brunostjohn.com`.
- Source extensions:
  - `.c` for C, `.cpp` for C++, `.h` for headers, `.mm` for Objective-C++, `.S` for assembly;
  - no `.inc`, `.cc`, `.cxx` or `.hpp`.
