:mod:`!importlib.version` --- Distribution version values
=========================================================

.. module:: importlib.version
   :synopsis: Parse, normalize and compare distribution versions.

.. versionadded:: 3.16

**Source code:** :source:`Modules/_versionmodule.c`

The :mod:`!importlib.version` module parses and compares distribution versions
using the `Version Specifiers specification`_.

.. _Version Specifiers specification: https://packaging.python.org/en/latest/specifications/version-specifiers/

.. class:: Version(version, /)

   Parse a version string into an immutable value.

   >>> from importlib.version import Version
   >>> version = Version("1.2.3")
   >>> str(version)
   '1.2.3'
   >>> version.release
   (1, 2, 3)
   >>> version < Version("2.0")
   True

   Alternate forms, leading zeros and surrounding whitespace are normalized.
   Invalid strings raise :exc:`InvalidVersion`.

   >>> value = Version("v1.02RC3+ABC_001")
   >>> str(value)
   '1.2rc3+abc.1'
   >>> value.pre
   ('rc', 3)
   >>> value.local
   'abc.1'

   Versions support ordering, equality, hashing and pickling. Trailing release
   zeros do not affect equality. Local labels participate in comparisons:

   >>> Version("1.0.0") == Version("1")
   True
   >>> Version("1+local") > Version("1")
   True
   >>> Version("1+local") == Version("1")
   False

   Specifier matching follows different rules: the specifier ``==1`` matches
   ``1+local``, although those version values are unequal.

   .. attribute:: epoch

      The nonnegative integer epoch, or zero when omitted.

   .. attribute:: release

      A nonempty tuple of nonnegative release integers, including trailing zeros.

   .. attribute:: pre

      A pair containing ``'a'``, ``'b'`` or ``'rc'`` and a nonnegative integer,
      or ``None`` when omitted.

   .. attribute:: post
                  dev

      The nonnegative post-release or development-release number, or ``None``.

   .. attribute:: local

      The normalized local label as a string, or ``None``.

   .. attribute:: major
                  minor
                  micro

      The first three release integers. Missing components are zero.

   .. attribute:: public

      The normalized version string without its local label.

   .. attribute:: base_version

      The epoch and release string, without pre-release, post-release,
      development-release or local components.

   .. attribute:: is_prerelease
                  is_postrelease
                  is_devrelease

      Whether the version contains a pre-release (including development),
      post-release or development-release component, respectively.

   .. classmethod:: from_parts(*, release, epoch=0, pre=None, post=None, dev=None, local=None)

      Construct a version from components, with the same normalization and
      validation as the string constructor. ``release`` must be a nonempty tuple
      of nonnegative integers; ``local`` is a string or ``None``. Subclass string
      constructors and initializers are not invoked.

   .. method:: __replace__(*, epoch=..., release=..., pre=..., post=..., dev=..., local=...)

      Return a version with the supplied components replaced. Omitted components
      retain their values; ``None`` resets an epoch to zero, a release to ``(0,)``
      or an optional component to its absent value. The result keeps the type of
      the original. If all components are unchanged after normalization, return
      the original object. A different release tuple produces a new object even
      when it compares equal.

      Supports :func:`copy.replace`.

.. exception:: InvalidVersion

   A subclass of :exc:`ValueError` raised for invalid version strings or
   components.
