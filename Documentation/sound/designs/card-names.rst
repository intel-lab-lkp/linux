.. SPDX-License-Identifier: GPL-2.0

==========
Card Names
==========

Background
==========

A sound card carries three strings that exist purely to be shown to a human:
``shortname``, ``longname`` and ``mixername`` in ``struct snd_card``, handed to
user space as ``name``, ``longname`` and ``mixername`` in
``struct snd_ctl_card_info``.  All three are fixed-size byte arrays, 32 and 80
bytes, and a name that does not fit is cut.

Those names are UTF-8.  That is not a new rule: USB has worked this way for
decades, because ``usb_string()`` converts the UTF-16LE string descriptors of a
device into UTF-8, so ``dev->product`` reaches the card name fields as UTF-8
already.

Two things follow from the fixed size, and this document is about both.

First, a cut can land in the middle of a multi-byte character, leaving a string
that is not valid UTF-8 and that user space cannot repair, because the missing
bytes never left the kernel.  ``snd_utf8_strscpy()`` fixes that: it is
``strscpy()`` except that it ends the copy after the last complete sequence
that fits.  Drivers that copy a device-provided name into one of these fields
should use it.

Second, cutting cleanly is still cutting.  32 bytes is eight CJK characters, so
a name can be valid and still be unusable.  For that, ``struct snd_card`` also
carries the untruncated form of each name:

======================  ==========================  ===============================
fixed field             untruncated form            user space type
======================  ==========================  ===============================
``shortname``           ``full_shortname``          ``SND_CTL_CARD_BTYPE_NAME``
``longname``            ``full_longname``           ``SND_CTL_CARD_BTYPE_LONGNAME``
``mixername``           ``full_mixername``          ``SND_CTL_CARD_BTYPE_MIXERNAME``
======================  ==========================  ===============================

This mirrors ``card->components``, which holds the whole components string of
which ``snd_ctl_card_info::components`` is a cut copy, and which user space
reads through the same ioctl.

``id``, ``driver`` and ``components`` have no untruncated counterpart of this
kind.  They are not display names: ``id`` is the unique handle of a card and
part of path names, ``driver`` is the key alsa-lib matches configuration
against, and ``components`` already has its own.  ``id`` in particular must
stay alphanumeric ASCII, and the kernel enforces that when deriving it.

Rules
=====

One name, two lengths
    The untruncated field is not a second name.  It is the same name, not cut.
    It stays NULL when the name fits, so "set" always means "the fixed field is
    not the whole story", and there is never a pair that could disagree.

Either may be empty
    A driver may fill only the fixed field, which is what nearly all of them do
    today, and a card may have no mixername at all.  No reader may assume a
    name is present.

Names are not identifiers
    Homoglyph detection is impossible in the kernel and NFC is not enforced, so
    two different cards can legitimately present names that look identical.
    Anything that matches, authorises or picks a device must use ``id``,
    ``driver`` or ``components``.

NFC is a recommendation
    Drivers should hand over names in Normalization Form C.  The kernel cannot
    normalise: that needs the Unicode tables of ``CONFIG_UNICODE``, which the
    sound core has no reason to depend on.  A reader that compares names should
    normalise them itself.

What the kernel guarantees, and what it does not
================================================

For every name it hands out, the kernel guarantees that the string:

* is well-formed UTF-8, and is never cut inside a multi-byte sequence -
  neither in the fixed field nor in the untruncated one;
* carries no code point that would break a line, that is no C0 or C1 control
  character and no U+2028 or U+2029.

The second one is not cosmetic: the kernel prints these names into
``/proc/asound/cards``, which is line based, so a newline in a name would
corrupt a file that user space parses.  That is also why the repair belongs in
the kernel rather than in alsa-lib.

Nothing beyond that is guaranteed, and that is a deliberate choice rather than
an oversight.  Invisible and bidirectional formatting characters, deprecated
format characters, the tag block, variation selectors and non-characters all
pass through untouched.  Catching them needs the Unicode character properties
(``Default_Ignorable_Code_Point`` and the general categories), which would mean
carrying a table in the sound core that goes stale with every Unicode release.
A hand-written list of ranges is worse than none: it looks like a defence while
leaving gaps, and user space then skips its own filtering.

So the division of labour is:

=====================================  ==============================
the kernel                             user space
=====================================  ==============================
well-formed UTF-8                      NFC normalisation
one line, no control characters        ``Default_Ignorable`` filtering
never cut inside a sequence            bidi isolation for display
=====================================  ==============================

Concretely, before showing a name, wrap it in U+2066/U+2069 so that strong
right-to-left characters cannot reorder the text around it, and drop
``Default_Ignorable_Code_Point``.

Rather than rejecting a name it dislikes, the kernel repairs it: a malformed
sequence becomes U+FFFD, one per bad byte as recommended for a maximal subpart
in Unicode TR#36, and a line breaking code point becomes a space.  Rejection
would achieve nothing here: the fixed field still holds the name, and readers
are told to use that whenever the untruncated one is absent, so a rejected name
would reach them anyway.

Driver side
===========

Copy a device-provided name into the fixed field with ``snd_utf8_strscpy()``,
then offer the untruncated form::

  snd_utf8_strscpy(card->shortname, product, sizeof(card->shortname));
  snd_card_set_full_shortname(card, product);

Order matters: the setter compares against ``card->shortname`` and stores
nothing if the name fit, so the fixed field has to be there first.  A driver
whose names are compile-time ASCII literals, which is most of them, needs
neither call.

The setters strip surrounding whitespace, cut an over-long name at a character
boundary, and treat an empty result as "unset".  Passing NULL clears a field.
They do not reject anything: the only error is ``-ENOMEM``.  The size limits
are four times the size of the corresponding fixed field
(``SNDRV_CARD_FULL_SHORTNAME_MAX`` and friends), so the same number of
characters always fits, whatever they encode to.

User space side
===============

The untruncated names are read through the existing
``SNDRV_CTL_IOCTL_CARD_BYTES`` ioctl, which already carries the components
string the same way.  No existing structure or ioctl changes, so an old binary
keeps working unchanged.

Call it once with ``data`` set to NULL to learn the length, then once with a
buffer of that size::

  struct snd_ctl_card_bytes b = { .type = SND_CTL_CARD_BTYPE_NAME };

  if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_BYTES, &b) < 0) {
          /* kernel too old: the fixed field is all there is */
          return use_fixed_name();
  }

  buf = malloc(b.data_len);
  if (!buf)
          return -ENOMEM;
  b.data_allocated = b.data_len;
  b.data = (__u64)(uintptr_t)buf;
  if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_BYTES, &b) < 0) {
          free(buf);
          return use_fixed_name();
  }

An empty result means the name fit, so ``snd_ctl_card_info`` already has it.
An ``-EINVAL`` from a kernel that does not know the type means the same thing.
``SNDRV_CTL_IOCTL_PVERSION`` reports at least 2.0.11 where the types exist.

There is deliberately no sysfs interface for now.  It would be a second piece
of ABI for the same data, and nothing in user space has asked for one yet; it
can be added later if udev rules turn out to need the names before the control
device is opened.
