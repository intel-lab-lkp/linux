# SPDX-License-Identifier: GPL-2.0

CONTEXT_ANALYSIS := 1

# The link order is important here. ufshcd-core must initialize
# before vendor drivers.
obj-$(CONFIG_SCSI_UFSHCD)	+= core/ host/
