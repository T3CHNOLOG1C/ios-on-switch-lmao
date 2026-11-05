
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

