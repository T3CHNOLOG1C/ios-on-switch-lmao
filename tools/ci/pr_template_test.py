"""The PR description check against the repository's own template."""

import re
import sys
import unittest
from pathlib import Path

from tools.ci import pr_template

TEMPLATE = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else ''

ANSWERS = {
    'What': 'Adds the SD card speed probe to the boot log.',
    'How': 'boot/loader/stamps.h gains a stamp read by SwitchSD at attach.',
    'Why': 'Slow cards were invisible until SpringBoard was late.',
    'Testing and evidence': 'bazel test //... passes; booted twice on an OLED, log lines attached.',
    'Risk': 'None at runtime: the stamp is read once and only logged.',
}


def _filled(template: str, answers: dict[str, str] = ANSWERS) -> str:
    body = template
    for name, answer in answers.items():
        body = re.sub(rf'(## {re.escape(name)}\n\n<!--.*?-->)', lambda m, a=answer: m.group(1) + '\n\n' + a, body)
    return body.replace('- [ ]', '- [x]')


class PrTemplateTest(unittest.TestCase):
    def test_a_filled_description_passes(self):
        self.assertEqual([], pr_template.problems(TEMPLATE, _filled(TEMPLATE)))

    def test_windows_line_endings_pass(self):
        self.assertEqual([], pr_template.problems(TEMPLATE, _filled(TEMPLATE).replace('\n', '\r\n')))

    def test_the_bare_template_fails_every_section_and_box(self):
        found = pr_template.problems(TEMPLATE, TEMPLATE)
        boxes = TEMPLATE.count('- [ ]')
        self.assertEqual(len(ANSWERS) + boxes, len(found))

    def test_an_empty_description_misses_every_section(self):
        found = pr_template.problems(TEMPLATE, '')
        self.assertTrue(all('missing' in p for p in found))
        self.assertEqual(len(ANSWERS) + 1, len(found))

    def test_a_placeholder_is_not_an_answer(self):
        body = _filled(TEMPLATE, {**ANSWERS, 'Risk': 'none'})
        self.assertEqual(['"Risk" needs a real answer, not "none".'], pr_template.problems(TEMPLATE, body))

    def test_the_prompt_is_not_an_answer(self):
        body = _filled(TEMPLATE, {**ANSWERS, 'Why': 'The problem it fixes, or what it makes possible.'})
        found = pr_template.problems(TEMPLATE, body)
        self.assertEqual(['"Why" repeats the template\'s prompt instead of answering it.'], found)

    def test_an_unticked_box_fails(self):
        body = _filled(TEMPLATE).replace('- [x]', '- [ ]', 1)
        found = pr_template.problems(TEMPLATE, body)
        self.assertEqual(1, len(found))
        self.assertIn('tick', found[0])

    def test_a_reworded_box_fails(self):
        body = _filled(TEMPLATE).replace('can explain it', 'mostly get it')
        found = pr_template.problems(TEMPLATE, body)
        self.assertEqual(1, len(found))
        self.assertIn('lacks the item', found[0])

    def test_a_deleted_section_fails(self):
        body = _filled(TEMPLATE).replace('## Why', '## Because')
        self.assertIn('The "Why" section is missing.', pr_template.problems(TEMPLATE, body))


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
