"""PEP 440 properties, with fixed examples for builds without Hypothesis."""

import operator
import pickle
import unittest

from importlib.version import InvalidVersion, Version
from test.support.hypothesis_helper import hypothesis

st = hypothesis.strategies
numbers = st.one_of(
    st.sampled_from([0, 1, 65535, 65536, 2**64, 2**128]),
    st.integers(min_value=0),
)
parts_strategy = st.fixed_dictionaries(
    {
        "epoch": numbers,
        "release": st.lists(numbers, min_size=1, max_size=8).map(tuple),
        "pre": st.one_of(
            st.none(), st.tuples(st.sampled_from(["a", "b", "rc"]), numbers)
        ),
        "post": st.one_of(st.none(), numbers),
        "dev": st.one_of(st.none(), numbers),
        "local": st.one_of(
            st.none(), st.sampled_from(["abc", "abc.1", "1.abc", "123"])
        ),
    }
)

COMPACT = dict(
    epoch=0,
    release=(1, 2, 0, 0),
    pre=("rc", 65535),
    post=0,
    dev=65535,
    local=None,
)
FULL = dict(
    epoch=2**80,
    release=(65536, 2, 0, 0, 0),
    pre=("b", 2**64),
    post=2**80,
    dev=2**64,
    local="abc.1",
)
STABLE = dict(epoch=0, release=(0,), pre=None, post=None, dev=None, local=None)


def format_parts(parts):
    """Spell normalized components independently of the native formatter."""
    text = ".".join(map(str, parts["release"]))
    if parts["epoch"]:
        text = f"{parts['epoch']}!{text}"
    if parts["pre"] is not None:
        letter, number = parts["pre"]
        text += f"{letter}{number}"
    if parts["post"] is not None:
        text += f".post{parts['post']}"
    if parts["dev"] is not None:
        text += f".dev{parts['dev']}"
    if parts["local"] is not None:
        text += f"+{parts['local']}"
    return text


def ordering_key(parts):
    """Order components directly using PEP 440 sentinels."""
    release = parts["release"]
    while release and release[-1] == 0:
        release = release[:-1]
    pre = parts["pre"]
    if pre is None:
        dev_only = parts["post"] is None and parts["dev"] is not None
        pre = (float("-inf") if dev_only else float("inf"),)
    else:
        pre = ({"a": 0, "b": 1, "rc": 2}[pre[0]], pre[1])
    post = -1 if parts["post"] is None else parts["post"]
    dev = float("inf") if parts["dev"] is None else parts["dev"]
    local = (
        ()
        if parts["local"] is None
        else tuple(
            (1, int(part)) if part.isdigit() else (0, part)
            for part in parts["local"].split(".")
        )
    )
    return parts["epoch"], release, pre, post, dev, local


class VersionProperties(unittest.TestCase):
    @hypothesis.given(parts=parts_strategy)
    @hypothesis.example(parts=COMPACT)
    @hypothesis.example(parts=FULL)
    @hypothesis.example(parts=STABLE)
    def test_components_and_string_roundtrip(self, parts):
        text = format_parts(parts)
        parsed = Version(text)
        constructed = Version.from_parts(**parts)
        self.assertEqual(str(parsed), text)
        self.assertEqual(str(constructed), text)
        for name, expected in parts.items():
            self.assertEqual(getattr(parsed, name), expected)
            self.assertEqual(getattr(constructed, name), expected)
        self.assertEqual(parsed, constructed)
        self.assertEqual(hash(parsed), hash(constructed))
        self.assertEqual(str(Version(str(parsed))), text)

    @hypothesis.given(left=parts_strategy, right=parts_strategy)
    @hypothesis.example(left=COMPACT, right=FULL)
    @hypothesis.example(left=STABLE, right=COMPACT)
    @hypothesis.example(left=STABLE, right=STABLE)
    def test_ordering_matches_component_oracle(self, left, right):
        a = Version(format_parts(left))
        b = Version(format_parts(right))
        ka, kb = ordering_key(left), ordering_key(right)
        for compare in (
            operator.lt,
            operator.le,
            operator.eq,
            operator.ne,
            operator.gt,
            operator.ge,
        ):
            self.assertEqual(compare(a, b), compare(ka, kb), compare.__name__)
        if ka == kb:
            self.assertEqual(hash(a), hash(b))

    @hypothesis.given(
        parts=parts_strategy, zeros=st.integers(min_value=1, max_value=8)
    )
    @hypothesis.example(parts=COMPACT, zeros=1)
    @hypothesis.example(parts=FULL, zeros=2)
    @hypothesis.example(parts=STABLE, zeros=4)
    def test_trailing_zero_equality(self, parts, zeros):
        a = Version.from_parts(**parts)
        release = parts["release"] + (0,) * zeros
        b = a.__replace__(release=release)
        self.assertEqual(a, b)
        self.assertEqual(hash(a), hash(b))
        self.assertEqual(b.release, release)
        self.assertIs(a.__replace__(), a)
        self.assertIsNot(b, a)

    @hypothesis.given(parts=parts_strategy, changes=parts_strategy)
    @hypothesis.example(parts=COMPACT, changes=FULL)
    @hypothesis.example(parts=FULL, changes=STABLE)
    def test_replace_components(self, parts, changes):
        original = Version.from_parts(**parts)
        for name, value in changes.items():
            expected = parts | {name: value}
            result = original.__replace__(**{name: value})
            self.assertEqual(str(result), format_parts(expected), name)
            self.assertEqual(result, Version.from_parts(**expected), name)
        self.assertEqual(str(original), format_parts(parts))

    @hypothesis.given(
        parts=parts_strategy,
        protocol=st.integers(min_value=0, max_value=pickle.HIGHEST_PROTOCOL),
    )
    @hypothesis.example(parts=COMPACT, protocol=0)
    @hypothesis.example(parts=FULL, protocol=pickle.HIGHEST_PROTOCOL)
    def test_pickle_roundtrip(self, parts, protocol):
        original = Version.from_parts(**parts)
        restored = pickle.loads(pickle.dumps(original, protocol))
        self.assertIs(type(restored), Version)
        self.assertEqual(str(restored), str(original))
        self.assertEqual(hash(restored), hash(original))

    @hypothesis.given(
        number=numbers,
        alias=st.sampled_from(
            [
                ("alpha", "a"),
                ("beta", "b"),
                ("c", "rc"),
                ("pre", "rc"),
                ("preview", "rc"),
            ]
        ),
        separator=st.sampled_from(["", ".", "_", "-"]),
    )
    @hypothesis.example(number=65536, alias=("preview", "rc"), separator="_")
    @hypothesis.example(number=0, alias=("alpha", "a"), separator="")
    def test_normalization(self, number, alias, separator):
        alternative, normalized = alias
        text = (
            f" \tV001.02{separator}{alternative.upper()}{separator}00{number}\n"
        )
        value = Version(text)
        expected = Version(f"1.2{normalized}{number}")
        self.assertEqual(str(value), str(expected))
        self.assertEqual(hash(value), hash(expected))
        self.assertEqual(Version("1.0+ABC_001"), Version("1+abc.1"))
        self.assertGreater(Version("1+abc"), Version("1"))
        self.assertGreater(Version("1+1"), Version("1+abc"))

    @hypothesis.given(parts=parts_strategy)
    @hypothesis.example(parts=COMPACT)
    @hypothesis.example(parts=FULL)
    def test_embedded_invalid_character_is_rejected(self, parts):
        text = format_parts(parts)
        for invalid in ("@" + text, text + "@", text + "\x00"):
            with self.assertRaises(InvalidVersion, msg=invalid):
                Version(invalid)


if __name__ == "__main__":
    unittest.main()
