"""The layout move's rewrite rules: old labels and paths to new ones.

The move applies them to every tracked text file; //tools/repath applies the same ones to a patch made before the move
(its diff headers and its hunks).
"""

import json
import re
from collections.abc import Callable
from pathlib import Path

OLD_TREE = 'out/native-a57/root-v1/tree'
PASS = 'out/native-a57/root-v1/passes/native-root-smp-v1'
_T = re.escape(OLD_TREE)

# (pattern, replacement), applied in order.
RULES = [
    # Files of the tree package by label.
    (re.compile(r'//' + _T + r":(drivers|kits)/([^/:\"'\s]+)/"), r'//\1/\2:'),
    (re.compile(r'//' + _T + r':diagnostics/ioswitch/'), r'//diagnostics/ioswitch:'),
    (re.compile(r'//' + _T + r':overlay/'), r'//overlay:'),
    # The tree's own subpackages (drivers/NativeMetal, drivers/TegraGM20BMetal, their tests).
    (re.compile(r'//' + _T + r'/(drivers|kits|diagnostics|overlay)/'), r'//\1/'),
    (re.compile(r'//' + re.escape(PASS) + r':build-options\.json'), '//kernel:build-options.json'),
    # Paths.
    (re.compile(_T + r'/(drivers|kits|diagnostics|overlay)\b'), r'\1'),
    (re.compile(_T + r'/package\b'), 'third_party/unixnu'),
    (re.compile(re.escape(PASS) + r'/build-options\.json'), 'kernel/build-options.json'),
    # native/ -> boot/ (after S3): the boot entry's sources in the package that builds them.
    (re.compile(r'//native:'), '//boot:'),
    (re.compile(r'//native/'), '//boot/'),
    (re.compile(r'"//native"'), '"//boot"'),
    (
        re.compile(
            r'(?<![A-Za-z0-9_./-])native/(cpu|display|handoff|installer|inventory|loader|probe|storage|BUILD\.bazel)\b'
        ),
        r'boot/\1',
    ),
]


def target_rule(labels: dict[str, str]) -> tuple[re.Pattern[str], Callable[[re.Match[str]], str]]:
    """Return the rule for //OLD_TREE:<target>: the target's new label (labels.json from the move)."""
    pattern = re.compile(r'//' + _T + r':([A-Za-z0-9_.+-]+)')

    def replace(match: re.Match[str]) -> str:
        return labels.get(match.group(1), match.group(0))

    return pattern, replace


def load_labels(path: Path) -> dict[str, str]:
    """Read the move's old target name -> new label table."""
    return json.loads(Path(path).read_text())


def rewrite(text: str, labels: dict[str, str]) -> str:
    """Rewrite every old label and path in text for the new layout.

    The tree package as a whole (labels like //out/native-a57/root-v1/tree:all) has no successor: left for review.
    """
    for pattern, replacement in RULES:
        text = pattern.sub(replacement, text)
    pattern, replace = target_rule(labels)
    return pattern.sub(replace, text)
