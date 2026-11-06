"""Checks a pull request's description (stdin) against the PR template: every section answered, every box ticked."""

import re
import sys
from pathlib import Path

_COMMENT = re.compile(r'<!--.*?-->', re.DOTALL)
_HEADING = re.compile(r'^## +(.+?)[ \t]*$', re.MULTILINE)
_BOX = re.compile(r'^[ \t]*[-*] \[([ xX])\] +(.+?)[ \t]*$', re.MULTILINE)
_NON_ANSWERS = {'-', '.', '...', 'idk', 'n/a', 'na', 'no', 'none', 'nothing', 'see title', 'tbd', 'todo', 'wip'}
_MIN_WORDS = 3


def _sections(markdown: str) -> dict[str, str]:
    parts = _HEADING.split(_COMMENT.sub('', markdown.replace('\r\n', '\n')))
    return {parts[i]: parts[i + 1].strip() for i in range(1, len(parts), 2)}


def _boxes(text: str) -> dict[str, bool]:
    return {' '.join(item.split()): mark != ' ' for mark, item in _BOX.findall(text)}


def _answer_problem(name: str, answer: str, prompts: set[str]) -> str | None:
    if not answer:
        return f'"{name}" is empty.'
    if ' '.join(answer.split()) in prompts:
        return f'"{name}" repeats the template\'s prompt instead of answering it.'
    if answer.lower().strip(' .!') in _NON_ANSWERS or len(answer.split()) < _MIN_WORDS:
        return f'"{name}" needs a real answer, not "{answer}".'
    return None


def problems(template: str, body: str) -> list[str]:
    """Return what the description gets wrong, one sentence each; empty when it passes."""
    found = []
    answers = _sections(body)
    prompts = {' '.join(c.split()) for c in re.findall(r'<!--(.*?)-->', template, re.DOTALL)}
    for name, prompt in _sections(template).items():
        if name not in answers:
            found.append(f'The "{name}" section is missing.')
            continue
        required = _boxes(prompt)
        if not required:
            found.append(_answer_problem(name, answers[name], prompts))
            continue
        ticked = _boxes(answers[name])
        for item in required:
            if item not in ticked:
                found.append(f'"{name}" lacks the item "{item}" (keep the items as the template words them).')
            elif not ticked[item]:
                found.append(f'"{name}": tick "{item}".')
    return [p for p in found if p]


def main() -> int:
    """Print the problems and fail when there are any: `pr_template.py TEMPLATE < body`."""
    found = problems(Path(sys.argv[1]).read_text(), sys.stdin.read())
    for problem in found:
        print(problem)
    if found:
        print('\nEdit the pull request description to fix these; the check reruns on every edit.')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
