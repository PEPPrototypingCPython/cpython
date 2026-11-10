import copyreg
import gc
import inspect
import operator
import pickle
import sys
import threading
import unittest
import weakref

from test.support import run_in_subinterp, script_helper, threading_helper

from importlib.version import InvalidVersion, Version
from concurrent.futures import ThreadPoolExecutor


ORDERED_VERSIONS = [
    "1.0.dev0",
    "1.0.dev456",
    "1.0.dev456+local",
    "1.0a0",
    "1.0a0.post0.dev0",
    "1.0a0.post0",
    "1.0a1.dev1",
    "1.0a1.dev1+local",
    "1.0a1",
    "1.0a1+local",
    "1.0b0",
    "1.0b1.dev456",
    "1.0b2",
    "1.0b2.post345.dev456",
    "1.0b2.post345",
    "1.0b2-346",
    "1.0rc0",
    "1.0rc1.dev1",
    "1.0c1",
    "1.0rc2",
    "1.0",
    "1.0.post0.dev0",
    "1.0.post0",
    "1.0.post456.dev34",
    "1.0.post456",
    "1.0.post456+local",
    "1.0.1.dev1",
    "1.0.1a1",
    "1.0.1",
    "1.0.1+local",
    "1.0.1.post1",
    "1.1.dev1",
    "1.2+a",
    "1.2+abc",
    "1.2+abcdef",
    "1.2+def",
    "1.2+0",
    "1.2+1",
    "1.2+1.abc",
    "1.2+1.1",
    "1.2+1.1.0",
    "1.2+2",
    "1.2+123",
    "1.2+123456",
    "1.2.r32+123456",
    "1.2.rev33+123456",
    "1!1.0.dev0",
    "1!1.0.dev456",
    "1!1.0.dev456+local",
    "1!1.0a0",
    "1!1.0a0.post0.dev0",
    "1!1.0a0.post0",
    "1!1.0a1.dev1",
    "1!1.0a1.dev1+local",
    "1!1.0a1",
    "1!1.0a1+local",
    "1!1.0b0",
    "1!1.0b1.dev456",
    "1!1.0b2",
    "1!1.0b2.post345.dev456",
    "1!1.0b2.post345",
    "1!1.0b2-346",
    "1!1.0rc0",
    "1!1.0rc1.dev1",
    "1!1.0c1",
    "1!1.0rc2",
    "1!1.0",
    "1!1.0.post0.dev0",
    "1!1.0.post0",
    "1!1.0.post456.dev34",
    "1!1.0.post456",
    "1!1.0.post456+local",
    "1!1.0.1.dev1",
    "1!1.0.1a1",
    "1!1.0.1",
    "1!1.0.1+local",
    "1!1.0.1.post1",
    "1!1.1.dev1",
    "1!1.2+a",
    "1!1.2+abc",
    "1!1.2+abcdef",
    "1!1.2+def",
    "1!1.2+0",
    "1!1.2+1",
    "1!1.2+1.abc",
    "1!1.2+1.1",
    "1!1.2+1.1.0",
    "1!1.2+2",
    "1!1.2+123",
    "1!1.2+123456",
    "1!1.2.r32+123456",
    "1!1.2.rev33+123456",
]

INVALID_VERSIONS = [
    "french toast",
    "1.0+a+",
    "1.0++",
    "1.0+_foobar",
    "1.0+foo&asd",
    "1.0+1+1",
    "1. 0",
    "1 .0",
    "1. 0a1",
    "1 .0a1",
    "1.0 a1",
    "1.0a 1",
    "٠١٢.٣٤٥.٦٧٨٩",
    ".",
    "..",
    "1..0",
    "1.0.",
    ".1.0",
    "1..2.3",
    "1.0+İ",
]

NORMALIZED_VERSIONS = [
    ("1.0dev", "1.0.dev0"),
    ("1.0.dev", "1.0.dev0"),
    ("1.0dev1", "1.0.dev1"),
    ("1.0-dev", "1.0.dev0"),
    ("1.0-dev1", "1.0.dev1"),
    ("1.0DEV", "1.0.dev0"),
    ("1.0.DEV", "1.0.dev0"),
    ("1.0DEV1", "1.0.dev1"),
    ("1.0.DEV1", "1.0.dev1"),
    ("1.0-DEV", "1.0.dev0"),
    ("1.0-DEV1", "1.0.dev1"),
    ("1.0a", "1.0a0"),
    ("1.0.a", "1.0a0"),
    ("1.0.a1", "1.0a1"),
    ("1.0-a", "1.0a0"),
    ("1.0-a1", "1.0a1"),
    ("1.0alpha", "1.0a0"),
    ("1.0.alpha", "1.0a0"),
    ("1.0.alpha1", "1.0a1"),
    ("1.0-alpha", "1.0a0"),
    ("1.0-alpha1", "1.0a1"),
    ("1.0A", "1.0a0"),
    ("1.0.A", "1.0a0"),
    ("1.0.A1", "1.0a1"),
    ("1.0-A", "1.0a0"),
    ("1.0-A1", "1.0a1"),
    ("1.0ALPHA", "1.0a0"),
    ("1.0.ALPHA", "1.0a0"),
    ("1.0.ALPHA1", "1.0a1"),
    ("1.0-ALPHA", "1.0a0"),
    ("1.0-ALPHA1", "1.0a1"),
    ("1.0b", "1.0b0"),
    ("1.0.b", "1.0b0"),
    ("1.0.b1", "1.0b1"),
    ("1.0-b", "1.0b0"),
    ("1.0-b1", "1.0b1"),
    ("1.0beta", "1.0b0"),
    ("1.0.beta", "1.0b0"),
    ("1.0.beta1", "1.0b1"),
    ("1.0-beta", "1.0b0"),
    ("1.0-beta1", "1.0b1"),
    ("1.0B", "1.0b0"),
    ("1.0.B", "1.0b0"),
    ("1.0.B1", "1.0b1"),
    ("1.0-B", "1.0b0"),
    ("1.0-B1", "1.0b1"),
    ("1.0BETA", "1.0b0"),
    ("1.0.BETA", "1.0b0"),
    ("1.0.BETA1", "1.0b1"),
    ("1.0-BETA", "1.0b0"),
    ("1.0-BETA1", "1.0b1"),
    ("1.0c", "1.0rc0"),
    ("1.0.c", "1.0rc0"),
    ("1.0.c1", "1.0rc1"),
    ("1.0-c", "1.0rc0"),
    ("1.0-c1", "1.0rc1"),
    ("1.0rc", "1.0rc0"),
    ("1.0.rc", "1.0rc0"),
    ("1.0.rc1", "1.0rc1"),
    ("1.0-rc", "1.0rc0"),
    ("1.0-rc1", "1.0rc1"),
    ("1.0C", "1.0rc0"),
    ("1.0.C", "1.0rc0"),
    ("1.0.C1", "1.0rc1"),
    ("1.0-C", "1.0rc0"),
    ("1.0-C1", "1.0rc1"),
    ("1.0RC", "1.0rc0"),
    ("1.0.RC", "1.0rc0"),
    ("1.0.RC1", "1.0rc1"),
    ("1.0-RC", "1.0rc0"),
    ("1.0-RC1", "1.0rc1"),
    ("1.0post", "1.0.post0"),
    ("1.0.post", "1.0.post0"),
    ("1.0post1", "1.0.post1"),
    ("1.0-post", "1.0.post0"),
    ("1.0-post1", "1.0.post1"),
    ("1.0POST", "1.0.post0"),
    ("1.0.POST", "1.0.post0"),
    ("1.0POST1", "1.0.post1"),
    ("1.0r", "1.0.post0"),
    ("1.0rev", "1.0.post0"),
    ("1.0.POST1", "1.0.post1"),
    ("1.0.r1", "1.0.post1"),
    ("1.0.rev1", "1.0.post1"),
    ("1.0-POST", "1.0.post0"),
    ("1.0-POST1", "1.0.post1"),
    ("1.0-5", "1.0.post5"),
    ("1.0-r5", "1.0.post5"),
    ("1.0-rev5", "1.0.post5"),
    ("1.0+AbC", "1.0+abc"),
    ("1.01", "1.1"),
    ("1.0a05", "1.0a5"),
    ("1.0b07", "1.0b7"),
    ("1.0c056", "1.0rc56"),
    ("1.0rc09", "1.0rc9"),
    ("1.0.post000", "1.0.post0"),
    ("1.1.dev09000", "1.1.dev9000"),
    ("00!1.2", "1.2"),
    ("0100!0.0", "100!0.0"),
    ("v1.0", "1.0"),
    ("   v1.0\t\n", "1.0"),
    ("\u202f1.0\t\u2029\n ", "1.0"),
]


class DisplayVersion(Version):
    def __init__(self, text):
        super().__init__(text)
        self.text = text


class OwnedInt(int):
    pass


class DisplayInt(int):
    def __str__(self):
        return "number"


class FailingDisplayInt(int):
    def __str__(self):
        raise RuntimeError("component formatting failed")


class RequiredLabelVersion(Version):
    __slots__ = ("label",)

    def __new__(cls, text, label):
        value = object.__new__(cls)
        Version.__init__(value, text)
        return value

    def __init__(self, text, label):
        self.label = label


class MutableVersion(Version):
    __slots__ = ()
    __mutable_version__ = True


class VersionTests(unittest.TestCase):
    def test_compact_decimal_format_boundaries(self):
        numbers = (0, 1, 9, 10, 99, 100, 999, 1000, 9999, 10000, 65535)
        for number in numbers:
            for length in range(1, 5):
                release = (number,) * length
                base = ".".join([str(number)] * length)
                for letter in ("a", "b", "rc"):
                    with self.subTest(number=number, length=length, letter=letter):
                        value = Version.from_parts(
                            release=release, pre=(letter, number),
                            post=number, dev=number,
                        )
                        expected = f"{base}{letter}{number}.post{number}.dev{number}"
                        self.assertEqual(str(value), expected)
                        self.assertEqual(value.public, expected)
                        self.assertEqual(value.base_version, base)
                        self.assertEqual(repr(value), f"<Version({expected!r})>")

    def test_full_format_preserves_integer_subclass_display(self):
        value = Version.from_parts(
            epoch=DisplayInt(1), release=(DisplayInt(2),),
            pre=("rc", DisplayInt(3)), post=DisplayInt(4), dev=DisplayInt(5),
            local="local",
        )
        self.assertEqual(str(value), "number!numberrcnumber.postnumber.devnumber+local")
        self.assertEqual(value.public, "number!numberrcnumber.postnumber.devnumber")
        self.assertEqual(value.base_version, "number!number")

    def test_full_format_propagates_component_errors(self):
        value = Version.from_parts(release=(FailingDisplayInt(1),))
        for operation in (str, repr, lambda v: v.public, lambda v: v.base_version):
            with self.subTest(operation=operation):
                with self.assertRaisesRegex(RuntimeError, "component formatting failed"):
                    operation(value)

    def test_module_namespace(self):
        script_helper.assert_python_ok(
            "-I", "-c",
            "import importlib.util, importlib.version as version, sys; "
            "assert version.__spec__.origin == 'built-in'; "
            "assert version.Version.__module__ == 'importlib.version'; "
            "assert version.InvalidVersion.__module__ == 'importlib.version'; "
            "assert 'importlib.version' in sys.builtin_module_names; "
            "assert '_version' not in sys.builtin_module_names; "
            "assert importlib.util.find_spec('_version') is None",
        )

    def test_empty_subclass_restores_once(self):
        for text in ("1.2.3", "2!1.2.3rc4.post5.dev6+local.7"):
            with self.subTest(text=text):
                value = Version.__new__(DisplayVersion)
                for operation in (
                    str,
                    hash,
                    repr,
                    lambda v: v.release,
                    lambda v: v == Version("0"),
                    lambda v: v.__replace__(),
                ):
                    with self.assertRaisesRegex(ValueError, "not initialized"):
                        operation(value)
                with self.assertRaises(InvalidVersion):
                    value.__setstate__("bad")
                value.__setstate__(text)
                self.assertEqual(value, Version(text))
                self.assertEqual(hash(value), hash(Version(text)))
                with self.assertRaisesRegex(TypeError, "already initialized"):
                    value.__setstate__("0")
                self.assertEqual(str(value), text)
        with self.assertRaises(TypeError):
            Version()
        self.assertEqual(Version(version="1"), Version("1"))
        value = object.__new__(Version)
        value.__init__(version="1")
        self.assertEqual(value, Version("1"))
        with self.assertRaises(TypeError):
            Version("1", version="1")
        with self.assertRaises(TypeError):
            Version(text="1")

    def test_failed_initialization_can_be_retried(self):
        value = object.__new__(Version)
        with self.assertRaises(InvalidVersion):
            value.__init__("bad")
        with self.assertRaisesRegex(ValueError, "not initialized"):
            hash(value)
        value.__init__("2!1.2.3rc4+abc.5")
        self.assertEqual(value, Version(str(value)))
        self.assertEqual(hash(value), hash(Version(str(value))))
        with self.assertRaisesRegex(TypeError, "already initialized"):
            value.__init__("0")

    @threading_helper.requires_working_threading()
    def test_concurrent_initializers_publish_once(self):
        value = object.__new__(Version)
        barrier = threading.Barrier(2)
        texts = ("1.2.3", "2!65536.2.3.4.5rc6.post7.dev8+abc.9")

        def initialize(text):
            barrier.wait()
            try:
                value.__init__(text)
            except TypeError as error:
                self.assertIn("already initialized", str(error))
                return False
            return True

        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(initialize, text) for text in texts]
            initialized = [future.result() for future in futures]
        self.assertEqual(sum(initialized), 1)
        self.assertIn(str(value), texts)
        self.assertEqual(hash(value), hash(Version(str(value))))

    @threading_helper.requires_working_threading()
    def test_concurrent_compact_size(self):
        current = [object.__new__(Version)]
        expected = current[0].__sizeof__()
        barrier = threading.Barrier(2)
        done = threading.Event()

        def initialize():
            barrier.wait()
            try:
                for _ in range(10_000):
                    value = object.__new__(Version)
                    current[0] = value
                    value.__init__("1.2.3")
            finally:
                done.set()

        def observe():
            sizes = {current[0].__sizeof__()}
            barrier.wait()
            while not done.is_set():
                sizes.add(current[0].__sizeof__())
            return sizes

        with ThreadPoolExecutor(max_workers=2) as executor:
            observed = executor.submit(observe)
            initialized = executor.submit(initialize)
            initialized.result()
            self.assertEqual(observed.result(), {expected})

    def test_object_allocation_restoration(self):
        value = copyreg._reconstructor(Version, object, None)
        value.__setstate__("2!1.2.3rc4+abc.5")
        self.assertEqual(value, Version("2!1.2.3rc4+abc.5"))
        self.assertEqual(hash(value), hash(Version(str(value))))

    def test_mutation_uses_immutable_snapshot(self):
        value = MutableVersion("1.0")
        hash(value)
        snapshot = Version("2!65536.2.3.4.5rc6.post7.dev8+abc.9")
        value.__setstate__(snapshot)
        self.assertEqual(value, snapshot)
        self.assertEqual(hash(value), hash(snapshot))
        self.assertEqual(str(value), str(snapshot))
        for name in (
            "epoch",
            "release",
            "pre",
            "post",
            "dev",
            "local",
            "major",
            "minor",
            "micro",
            "public",
            "base_version",
            "is_prerelease",
            "is_postrelease",
            "is_devrelease",
        ):
            self.assertEqual(getattr(value, name), getattr(snapshot, name))
        self.assertIs(value.__replace__(), value)
        self.assertIs(value.__replace__(local=snapshot.local), value)
        replaced = value.__replace__(epoch=0, local=None)
        self.assertIs(type(replaced), MutableVersion)
        self.assertEqual(str(replaced), "65536.2.3.4.5rc6.post7.dev8")
        self.assertEqual(pickle.loads(pickle.dumps(value)), snapshot)
        with self.assertRaisesRegex(TypeError, "immutable"):
            snapshot.__setstate__(Version("0"))

    @threading_helper.requires_working_threading()
    def test_concurrent_snapshot_readers(self):
        value = MutableVersion("1.0")
        small = Version("1.0")
        full = Version("2!65536.2.3.4.5rc6.post7.dev8+abc.9")
        expected_text = {str(small), str(full)}
        expected_hash = {hash(small), hash(full)}

        def mutate():
            for _ in range(500):
                value.__setstate__(full)
                value.__setstate__(small)

        def read():
            for _ in range(500):
                self.assertIn(str(value), expected_text)
                self.assertIn(hash(value), expected_hash)
                self.assertEqual(value, value)
                # Force a new result: a no-op may return the concurrently mutable source.
                result = value.__replace__(epoch=3, local=None)
                self.assertIsNot(result, value)
                self.assertIn(str(result), {
                    str(small.__replace__(epoch=3, local=None)),
                    str(full.__replace__(epoch=3, local=None)),
                })

        with ThreadPoolExecutor(max_workers=3) as executor:
            futures = [
                executor.submit(mutate),
                executor.submit(read),
                executor.submit(read),
            ]
            for future in futures:
                future.result()

    def test_subinterpreter_state_is_independent(self):
        code = """
from importlib.version import Version, InvalidVersion
value = Version('2!1.2.3rc4+local.5')
assert value == Version(str(value))
assert hash(value) == hash(Version(str(value)))
try:
    Version('bad')
except InvalidVersion:
    pass
else:
    raise AssertionError('invalid version accepted')
"""
        for _ in range(3):
            self.assertEqual(run_in_subinterp(code), 0)

    def test_normalization_table(self):
        for text, expected in NORMALIZED_VERSIONS:
            with self.subTest(text=text):
                value = Version(text)
                self.assertEqual(str(value), expected)
                self.assertEqual(value, Version(expected))
                self.assertEqual(hash(value), hash(Version(expected)))

    def test_invalid_versions(self):
        for text in INVALID_VERSIONS:
            with self.subTest(text=text):
                with self.assertRaises(InvalidVersion):
                    Version(text)

    def test_ordering_table(self):
        values = [Version(text) for text in ORDERED_VERSIONS]
        self.assertEqual(sorted(reversed(values)), values)
        for i, left in enumerate(values):
            for j, right in enumerate(values):
                with self.subTest(left=left, right=right):
                    self.assertEqual(left < right, i < j)
                    self.assertEqual(left <= right, i <= j)
                    self.assertEqual(left == right, i == j)
                    self.assertEqual(left != right, i != j)
                    self.assertEqual(left >= right, i >= j)
                    self.assertEqual(left > right, i > j)

    def test_normalization_and_properties(self):
        cases = {
            "v1.0": "1.0",
            "1.0alpha1": "1.0a1",
            "1.0-c2": "1.0rc2",
            "1.0-3": "1.0.post3",
            "1.0DEV": "1.0.dev0",
            "1!2.0+ABC_001": "1!2.0+abc.1",
            "\u202f1.2.3\u2029": "1.2.3",
        }
        for text, normalized in cases.items():
            with self.subTest(text=text):
                value = Version(text)
                self.assertEqual(str(value), normalized)
                self.assertEqual(value, Version(normalized))
                self.assertEqual(hash(value), hash(Version(normalized)))

        value = Version("2!1.2.3rc4.post5.dev6+local.7")
        self.assertEqual(value.epoch, 2)
        self.assertEqual(value.release, (1, 2, 3))
        self.assertEqual(value.pre, ("rc", 4))
        self.assertEqual(value.post, 5)
        self.assertEqual(value.dev, 6)
        self.assertEqual(value.local, "local.7")
        self.assertEqual(value.public, "2!1.2.3rc4.post5.dev6")
        self.assertEqual(value.base_version, "2!1.2.3")

    def test_ordering_and_hash(self):
        texts = [
            "1.0.dev0",
            "1.0a0",
            "1.0a0.post0.dev0",
            "1.0a0.post0",
            "1.0b1",
            "1.0rc1",
            "1.0",
            "1.0+local",
            "1.0.post0.dev0",
            "1.0.post0",
            "2!0",
        ]
        values = list(map(Version, texts))
        self.assertEqual(values, sorted(reversed(values)))
        self.assertEqual(Version("1"), Version("1.0.0"))
        self.assertEqual(hash(Version("1")), hash(Version("1.0.0")))
        self.assertIs(Version("1").__eq__(object()), NotImplemented)
        self.assertIs(Version("1").__ne__(object()), NotImplemented)
        for op in (operator.lt, operator.le, operator.gt, operator.ge):
            with self.assertRaises(TypeError):
                op(Version("1"), object())

    def test_component_factories(self):
        value = Version.from_parts(
            epoch=2,
            release=(1, 2, 3),
            pre=("BETA", 4),
            post=5,
            dev=6,
            local="ABC_001",
        )
        self.assertEqual(str(value), "2!1.2.3b4.post5.dev6+abc.1")
        changed = value.__replace__(epoch=None, pre=None, local=None)
        self.assertEqual(str(changed), "1.2.3.post5.dev6")
        self.assertIs(changed.__replace__(), changed)
        self.assertIs(changed.__replace__(release=changed.release), changed)
        self.assertIsInstance(
            Version.from_parts(release=(65536,), post=2**80), Version
        )

    def test_invalid_components(self):
        invalid = (
            {"release": ()},
            {"release": (-1,)},
            {"release": [1]},
            {"release": (1,), "epoch": -1},
            {"release": (1,), "pre": ("alpha", -1)},
            {"release": (1,), "local": "abc..1"},
        )
        for parts in invalid:
            with self.subTest(parts=parts):
                with self.assertRaises(InvalidVersion):
                    Version.from_parts(**parts)

    def test_pickle_component_cycle(self):
        component = OwnedInt(65536)
        value = Version.from_parts(release=(component,))
        component.owner = value
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            with self.subTest(protocol=protocol):
                restored = pickle.loads(pickle.dumps(value, protocol))
                self.assertIs(restored.release[0].owner, restored)
                self.assertEqual(restored, value)

    def test_component_pickle_keeps_integer_types(self):
        values = (
            Version.from_parts(release=(True,), post=False),
            Version.from_parts(
                epoch=DisplayInt(2),
                release=(DisplayInt(3),),
                pre=("rc", DisplayInt(4)),
                post=DisplayInt(5),
                dev=DisplayInt(6),
            ),
        )
        for value in values:
            for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
                with self.subTest(value=value, protocol=protocol):
                    restored = pickle.loads(pickle.dumps(value, protocol))
                    self.assertEqual(str(restored), str(value))
                    self.assertEqual(restored, value)
                    self.assertEqual(hash(restored), hash(value))
                    self.assertIs(
                        type(restored.release[0]), type(value.release[0])
                    )
                    self.assertIs(type(restored.epoch), type(value.epoch))

    def test_required_subclass_pickle(self):
        value = RequiredLabelVersion("2!1.2rc3+abc.4", "required")
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            with self.subTest(protocol=protocol):
                restored = pickle.loads(pickle.dumps(value, protocol))
                self.assertIs(type(restored), RequiredLabelVersion)
                self.assertEqual(str(restored), str(value))
                self.assertEqual(hash(restored), hash(value))
                self.assertEqual(restored.label, "required")

    def test_subclass_and_pickle(self):
        value = DisplayVersion("1.2.3")
        restored = pickle.loads(pickle.dumps(value))
        self.assertIs(type(restored), DisplayVersion)
        self.assertEqual(restored.text, "1.2.3")
        self.assertEqual(restored, value)

    def test_empty_size_counts_only_allocated_storage(self):
        empty = object.__new__(Version)
        self.assertEqual(sys.getsizeof(empty), sys.getsizeof(Version("1")))

    def test_compact_type_cycle_is_collectible(self):
        def cycle():
            class TemporaryVersion(Version):
                __slots__ = ()
            TemporaryVersion.value = TemporaryVersion("1.2.3")
            return weakref.ref(TemporaryVersion)

        reference = cycle()
        gc.collect()
        self.assertIsNone(reference())

    def test_size_and_cycles(self):
        self.assertLess(
            sys.getsizeof(Version("1.2.3")), sys.getsizeof(Version("65536.1"))
        )

        class CyclicVersion(Version):
            __slots__ = ("__weakref__",)

        class Component(int):
            pass

        component = Component(65536)
        value = CyclicVersion.from_parts(release=(component,))
        component.owner = value
        reference = weakref.ref(value)
        del value, component
        gc.collect()
        self.assertIsNone(reference())

    def test_signatures(self):
        create = inspect.signature(Version.from_parts)
        change = inspect.signature(Version("1").__replace__)
        self.assertIs(
            create.parameters["release"].default, inspect.Parameter.empty
        )
        self.assertEqual(set(create.parameters), set(change.parameters))

    def test_callbacks_do_not_observe_partial_values(self):
        code = r"""
import copyreg
import gc
import sys
import threading
from importlib.version import Version
sys.set_int_max_str_digits(0)
def audit(event, args):
    if event == 'import' and args[0] == '_pylong':
        for value in gc.get_objects():
            if type(value) is tuple:
                try:
                    hash(value)
                except Exception:
                    pass
sys.addaudithook(audit)
value = Version('9' * 7000 + '.0')
assert value.release[0] == int('9' * 7000)
"""
        script_helper.assert_python_ok("-X", "faulthandler", "-c", code)


if __name__ == "__main__":
    unittest.main()
