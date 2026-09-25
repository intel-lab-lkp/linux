# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_SND_USB_JOCKEY3) += snd-reloop-jockey3.o
snd-reloop-jockey3-y := jockey3.o ploytec_proto.o ploytec_codec.o ploytec_midi.o

# The codec functions are not exported, so the KUnit tests are
# linked into the driver module rather than built as a separate test module.
snd-reloop-jockey3-$(CONFIG_SND_USB_JOCKEY3_CODEC_KUNIT_TEST) += ploytec_codec_kunit.o