/* SPDX-License-Identifier: GPL-2.0-or-later */

int mxl862xx_api_wrap(struct mxl862xx_priv *priv, u16 cmd, void *data, u16 size,
		      bool read, bool quiet);
int mxl862xx_reset(struct mxl862xx_priv *priv);
