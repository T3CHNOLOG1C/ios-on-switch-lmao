"""The fill of an SVG path as per-pixel coverage: exact area coverage of the flattened outline, nonzero winding.

The coverage pass is font-rs's signed-area accumulation (Raph Levien, Apache-2.0): each line adds its signed area to
the cells it crosses, and a running sum along each row is the coverage. Plain IEEE arithmetic only, so every host
rasterizes the same bytes.
"""

import itertools
import math
import re
from collections.abc import Iterator
from pathlib import Path

Point = tuple[float, float]
Segment = tuple[Point, ...]

_TOKEN = re.compile(r'[MmLlHhVvCcSsQqTtZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')
_ARITY = {'M': 2, 'L': 2, 'H': 1, 'V': 1, 'C': 6, 'S': 4, 'Q': 4, 'T': 2, 'Z': 0}
_FLATNESS = 1 / 256


def svg_path_data(svg: Path) -> str:
    """Read the d attribute of the document's one <path>."""
    found = re.findall(r'<path\b[^>]*\bd="([^"]*)"', svg.read_text())
    if len(found) != 1:
        raise ValueError(f'{svg}: expected exactly one <path d="...">, found {len(found)}')
    return found[0]


class _Pen:
    """Absolute segments from SVG path commands, the state relative and smooth commands need."""

    def __init__(self) -> None:
        self.at: Point = (0.0, 0.0)
        self.start: Point = (0.0, 0.0)
        self.control: Point | None = None
        self.contours: list[list[Segment]] = []

    def _reflected(self) -> Point:
        if self.control is None:
            return self.at
        return (2 * self.at[0] - self.control[0], 2 * self.at[1] - self.control[1])

    def _add(self, segment: Segment, control: Point | None) -> None:
        if not self.contours:
            self.contours.append([])
        self.contours[-1].append(segment)
        self.at, self.control = segment[-1], control

    def command(self, kind: str, args: list[float]) -> None:
        """Apply one absolute command (upper-case kind) to the pen."""
        x, y = self.at
        if kind == 'M':
            self.at = self.start = (args[0], args[1])
            self.contours.append([])
            self.control = None
        elif kind == 'Z':
            if self.at != self.start:
                self._add((self.at, self.start), None)
            self.at, self.control = self.start, None
        elif kind in 'LHV':
            to = {'L': (args[0], args[-1]), 'H': (args[0], y), 'V': (x, args[0])}[kind]
            self._add((self.at, to), None)
        elif kind in 'CS':
            first = (args[0], args[1]) if kind == 'C' else self._reflected()
            second, to = (args[-4], args[-3]), (args[-2], args[-1])
            self._add((self.at, first, second, to), second)
        else:
            control = (args[0], args[1]) if kind == 'Q' else self._reflected()
            self._add((self.at, control, (args[-2], args[-1])), control)


def _absolute(kind: str, args: list[float], at: Point) -> list[float]:
    if kind.isupper():
        return args
    if kind == 'h':
        return [args[0] + at[0]]
    if kind == 'v':
        return [args[0] + at[1]]
    return [value + at[i % 2] for i, value in enumerate(args)]


def contours(d: str) -> list[list[Segment]]:
    """Parse path data into contours of absolute line, quadratic and cubic segments."""
    tokens = _TOKEN.findall(d)
    pen, kind, i = _Pen(), '', 0
    while i < len(tokens):
        if tokens[i].isalpha():
            kind, i = tokens[i], i + 1
        if not kind:
            raise ValueError('path data does not start with a command')
        arity = _ARITY[kind.upper()]
        args = [float(t) for t in tokens[i : i + arity] if not t.isalpha()]
        if len(args) != arity or any(t.isalpha() for t in tokens[i : i + arity]):
            raise ValueError(f'path command {kind} needs {arity} numbers')
        pen.command(kind.upper(), _absolute(kind, args, pen.at))
        i += arity
        if kind in 'Mm':
            kind = 'L' if kind == 'M' else 'l'
        elif kind in 'Zz':
            kind = ''
    return [c for c in pen.contours if c]


def _flat(points: Segment) -> bool:
    (x0, y0), (x1, y1) = points[0], points[-1]
    dx, dy = x1 - x0, y1 - y0
    length = math.sqrt(dx * dx + dy * dy)
    for x, y in points[1:-1]:
        ex, ey = x - x0, y - y0
        off = abs(ex * dy - ey * dx) / length if length else math.sqrt(ex * ex + ey * ey)
        if off > _FLATNESS:
            return False
    return True


def _halves(points: Segment) -> tuple[Segment, Segment]:
    left, right, level = [points[0]], [points[-1]], list(points)
    while len(level) > 1:
        level = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2) for a, b in itertools.pairwise(level)]
        left.append(level[0])
        right.append(level[-1])
    return tuple(left), tuple(reversed(right))


def flatten(segment: Segment) -> Iterator[Point]:
    """Yield the end points of lines within _FLATNESS of a pixel of the segment (the start point excluded)."""
    stack = [segment]
    while stack:
        piece = stack.pop()
        if len(piece) == 2 or _flat(piece):
            yield piece[-1]
            continue
        left, right = _halves(piece)
        stack.extend((right, left))


class Coverage:
    """A width x height grid of signed areas; lines go in, coverage in [0, 1] comes out."""

    def __init__(self, width: int, height: int) -> None:
        """Make an empty width x height grid."""
        self.width, self.height = width, height
        self.cells = [[0.0] * (width + 2) for _ in range(height)]

    def line(self, p0: Point, p1: Point) -> None:
        """Add one directed edge of the outline."""
        if p0[1] == p1[1]:
            return
        direction = 1.0 if p0[1] < p1[1] else -1.0
        if direction < 0:
            p0, p1 = p1, p0
        if min(p0[0], p1[0]) < 0 or max(p0[0], p1[0]) > self.width or p0[1] < 0 or p1[1] > self.height:
            raise ValueError('outline leaves the canvas')
        slope = (p1[0] - p0[0]) / (p1[1] - p0[1])
        x = p0[0]
        for row in range(math.floor(p0[1]), min(self.height, math.ceil(p1[1]))):
            dy = min(row + 1.0, p1[1]) - max(float(row), p0[1])
            x_next = x + slope * dy
            self._span(self.cells[row], x, x_next, dy * direction)
            x = x_next

    @staticmethod
    def _span(cells: list[float], x: float, x_next: float, d: float) -> None:
        x0, x1 = min(x, x_next), max(x, x_next)
        x0i, x1i = math.floor(x0), math.ceil(x1)
        if x1i <= x0i + 1:
            mid = 0.5 * (x + x_next) - x0i
            cells[x0i] += d - d * mid
            cells[x0i + 1] += d * mid
            return
        s = 1 / (x1 - x0)
        x0f = x0 - x0i
        a0 = 0.5 * s * (1 - x0f) * (1 - x0f)
        x1f = x1 - x1i + 1
        am = 0.5 * s * x1f * x1f
        cells[x0i] += d * a0
        if x1i == x0i + 2:
            cells[x0i + 1] += d * (1 - a0 - am)
        else:
            a1 = s * (1.5 - x0f)
            cells[x0i + 1] += d * (a1 - a0)
            for xi in range(x0i + 2, x1i - 1):
                cells[xi] += d * s
            a2 = a1 + (x1i - x0i - 3) * s
            cells[x1i - 1] += d * (1 - a2 - am)
        cells[x1i] += d * am

    def levels(self) -> list[bytes]:
        """Return the rows of coverage as 0-255 (round half up)."""
        rows = []
        for cells in self.cells:
            total, row = 0.0, bytearray(self.width)
            for x in range(self.width):
                total += cells[x]
                row[x] = int(min(abs(total), 1.0) * 255 + 0.5)
            rows.append(bytes(row))
        return rows


def rasterize(d: str, width: int, height: int, scale: float, offset: Point) -> list[bytes]:
    """Fill the path at canvas = scale * path + offset: rows of 0-255 coverage."""
    grid = Coverage(width, height)
    for contour in contours(d):
        placed = [tuple((scale * x + offset[0], scale * y + offset[1]) for x, y in seg) for seg in contour]
        at = placed[0][0]
        for segment in placed:
            for point in flatten(segment):
                grid.line(at, point)
                at = point
        grid.line(at, placed[0][0])
    return grid.levels()
