.. SPDX-License-Identifier: GPL-2.0

.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/maintainer/configure-git.rst

:译者:

 吴想成 Wu XiangCheng <bobwxc@email.cn>
 袁维杰 Weijie Yuan <wy@wyuan.org>

配置 Git
========

本章介绍维护者级别的 Git 配置。

拉取请求中使用的带标签分支（参见 Documentation/maintainer/pull-requests.rst）
应使用开发者的 GPG 公钥签名。向 ``git tag`` 传递 ``-u <key-id>`` 可以创建签名标签。
不过，由于你对同一项目 *通常* 会使用同一个密钥，因此可以在配置中设置该密钥并使用 ``-s``
选项。使用以下命令设置默认的 ``key-id``::

	git config user.signingkey "keyname"

也可以手动编辑你的 ``.git/config`` 或 ``~/.gitconfig`` 文件::

	[user]
		name = Jane Developer
		email = jd@domain.org
		signingkey = jd@domain.org

你可能需要让 ``git`` 使用 ``gpg2``::

	[gpg]
		program = /path/to/gpg2

你可能还需要告诉 ``gpg`` 使用哪个 ``tty`` （将其添加到 shell 的 rc 文件中）::

	export GPG_TTY=$(tty)
