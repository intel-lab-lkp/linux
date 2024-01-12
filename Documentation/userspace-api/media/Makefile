# SPDX-License-Identifier: GPL-2.0

# Rules to convert a .h file to inline RST documentation

SRC_DIR=$(srctree)/Documentation/userspace-api/media
PARSER = $(srctree)/Documentation/sphinx/parse-headers.pl
UAPI = $(srctree)/include/uapi/linux
KAPI = $(srctree)/include/linux

FILES = ca.h.rst dmx.h.rst frontend.h.rst net.h.rst \
	videodev2.h.rst media.h.rst cec.h.rst lirc.h.rst

TARGETS := $(addprefix $(BUILDDIR)/, $(FILES))

cmd_parse_headers = \
	echo ${PARSER} $< $@ $(SRC_DIR)/$(notdir $@).exceptions; \
	${PARSER} $< $@ $(SRC_DIR)/$(notdir $@).exceptions

quiet_cmd_parse_headers = echo '  PARSE   $(patsubst $(srctree)/%,%,$<)'; \
	${PARSER} $< $@ $(SRC_DIR)/$(notdir $@).exceptions

silent_cmd_parse_headers = ${cmd_parse_headers}

$(BUILDDIR)/ca.h.rst: ${UAPI}/dvb/ca.h ${PARSER} $(SRC_DIR)/ca.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/dmx.h.rst: ${UAPI}/dvb/dmx.h ${PARSER} $(SRC_DIR)/dmx.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/frontend.h.rst: ${UAPI}/dvb/frontend.h ${PARSER} $(SRC_DIR)/frontend.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/net.h.rst: ${UAPI}/dvb/net.h ${PARSER} $(SRC_DIR)/net.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/videodev2.h.rst: ${UAPI}/videodev2.h ${PARSER} $(SRC_DIR)/videodev2.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/media.h.rst: ${UAPI}/media.h ${PARSER} $(SRC_DIR)/media.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/cec.h.rst: ${UAPI}/cec.h ${PARSER} $(SRC_DIR)/cec.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

$(BUILDDIR)/lirc.h.rst: ${UAPI}/lirc.h ${PARSER} $(SRC_DIR)/lirc.h.rst.exceptions
	@$($(quiet)cmd_parse_headers)

# Media build rules

.PHONY: all html texinfo epub xml latex

all: $(IMGDOT) $(BUILDDIR) ${TARGETS}
html: all
texinfo: all
epub: all
xml: all
latex: $(IMGPDF) all
linkcheck:

clean:
	-rm -f $(DOTTGT) $(IMGTGT) ${TARGETS} 2>/dev/null

$(BUILDDIR):
	$(Q)mkdir -p $@
