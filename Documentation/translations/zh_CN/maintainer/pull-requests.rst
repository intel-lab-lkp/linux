.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/maintainer/pull-requests.rst

:译者:

 吴想成 Wu XiangCheng <bobwxc@email.cn>
 袁维杰 Weijie Yuan <wy@wyuan.org>

如何创建拉取请求
================

本章介绍维护者如何创建拉取请求并将其提交给其他维护者。这适用于将更改从一棵
维护者树转移到另一棵维护者树。

本文档由 Tobin C. Harding（当时他尚不是一名经验丰富的维护者）编写，主要依据
Greg Kroah-Hartman 和 Linus Torvalds 在 LKML 上的评论。Jonathan Corbet 和
Mauro Carvalho Chehab 提出了建议和修正。曲解原意并非有意，却在所难免；请将
责难发送给 Tobin C. Harding <me@tobin.cc>。

原始邮件线程::

	https://lore.kernel.org/r/20171114110500.GA21175@kroah.com


创建分支
--------

首先，你需要将希望包含在拉取请求中的所有更改放在一个单独的分支上。通常，这个
分支会基于你准备向其发送拉取请求的开发者树中的某个分支。

要创建拉取请求，必须先给刚刚创建的分支打标签。建议选择一个有意义的标签名称，
使你和其他人在一段时间后仍能理解其含义。一种良好做法是在名称中指明来源子系统
和目标内核版本。

Greg 给出了以下示例。对于一个包含 drivers/char 杂项、准备应用到 4.15-rc1
内核版本的拉取请求，可以将其命名为 ``char-misc-4.15-rc1``。如果要从名为
``char-misc-next`` 的分支创建该标签，可以使用以下命令::

	git tag -s char-misc-4.15-rc1 char-misc-next

该命令会基于 ``char-misc-next`` 分支的最后一个提交创建名为
``char-misc-4.15-rc1`` 的签名标签，并使用你的 GPG 密钥签名（参见
Documentation/translations/zh_CN/maintainer/configure-git.rst）。

Linus 只接受基于签名标签的拉取请求。其他维护者的要求可能不同。

运行上述命令时，``git`` 会打开编辑器并要求你描述该标签。在本例中，你描述的是
一个拉取请求，因此应概述其中包含什么、为何应当合并，以及做过哪些测试（如有）。
所有这些信息都会保存在标签本身中；如果维护者合并了拉取请求，它们还会进入维护者
创建的合并提交。因此请认真撰写，因为它将永远保留在内核树中。

正如 Linus 所说::

	总之，至少对我而言，重要的是 *说明文字*。我想知道自己
	正在拉取什么，以及为什么应当拉取。我还希望把这段说明用作合并说明，
	所以它不但要让我看得明白，还应当能成为有意义的历史记录。

	请注意，如果拉取请求中有什么异常，就非常应该在说明中写清楚。
	如果你改动了自己并不维护的文件，请解释 _为什么_。无论如何，我会在
	差异统计中看到它；如果你没提到，我只会更加怀疑。当你在合并窗口
	结束后给我发送新内容（甚至是错误修复，但看起来很吓人的那种）时，
	不仅要解释它们做了什么、为什么这样做，还要解释这个 _时机_。
	发生了什么，导致它没能通过合并窗口进入……

	我会采用你写在拉取请求邮件 _和_ 签名标签中的内容。因此，
	取决于你的工作流程，你可以在签名标签中描述自己的工作（这些内容也会
	自动进入拉取请求邮件），也可以让签名标签仅仅作为一个没有实质内容的
	占位符，等到真正向我发送拉取请求时再描述这项工作。

	没错，我会编辑这段说明。一方面是因为我通常会做些简单的格式调整（整体
	缩进、引用等）；另一方面，其中一些内容在我拉取时可能很有用（例如描述
	冲突，以及你此时发送请求所面临的个人问题），但放在合并提交说明的上下文
	中可能没有意义，所以我会尽量使它通顺。我也会修正看到的拼写错误和糟糕
	语法，尤其是非英语母语者写的内容（英语母语者也一样 ;^）。
	不过，我也可能会漏掉一些，甚至再添上一些。

			Linus

Greg 给出了一个拉取请求示例::

	Char/Misc patches for 4.15-rc1

	Here is the big char/misc patch set for the 4.15-rc1 merge window.
	Contained in here is the normal set of new functions added to all
	of these crazy drivers, as well as the following brand new
	subsystems:
		- time_travel_controller: Finally a set of drivers for the
		  latest time travel bus architecture that provides i/o to
		  the CPU before it asked for it, allowing uninterrupted
		  processing
		- relativity_shifters: due to the affect that the
		  time_travel_controllers have on the overall system, there
		  was a need for a new set of relativity shifter drivers to
		  accommodate the newly formed black holes that would
		  threaten to suck CPUs into them.  This subsystem handles
		  this in a way to successfully neutralize the problems.
		  There is a Kconfig option to force these to be enabled
		  when needed, so problems should not occur.

	All of these patches have been successfully tested in the latest
	linux-next releases, and the original problems that it found have
	all been resolved (apologies to anyone living near Canberra for the
	lack of the Kconfig options in the earlier versions of the
	linux-next tree creations.)

	Signed-off-by: Your-name-here <your_email@domain>


标签说明的格式与 Git 提交说明相同：顶部用一行作为“摘要主题”，
并确保在底部添加签署信息。

现在本地已有签名标签，需要将其推送到可供拉取的位置::

	git push origin char-misc-4.15-rc1


创建拉取请求
------------

最后要做的是编写拉取请求消息。``git`` 可以方便地用 ``git request-pull``
命令代劳，但需要获得一些帮助，以确定你希望对方拉取什么，以及拉取内容应以什么为
基础（从而显示正确的待拉取更改和差异统计）。以下命令会生成一个拉取请求::

	git request-pull master git://git.kernel.org/pub/scm/linux/kernel/git/gregkh/char-misc.git/ char-misc-4.15-rc1

引用 Greg 的话::

	这个命令要求 Git 比较“char-misc-4.15-rc1”标签所在位置与“master”分支
	顶端之间的差异（在我的例子中，“master”指向我与 Linus 的树发生分叉前
	的最后位置，通常是一个 -rc 版本），并使用 git:// 协议拉取。如果希望使用
	https://，也可以在这里使用（但请注意，一些位于防火墙后的用户使用 HTTPS
	方式拉取 Git 仓库时会遇到问题）。

	如果请求拉取的仓库中没有“char-misc-4.15-rc1”标签，Git 会抱怨说找不到
	它。这可以方便地提醒你，确实需要把标签推送到一个公开位置。

	“git request-pull”的输出会包含要拉取的 Git 树位置和具体标签，以及
	该标签的完整说明文字（这正是需要在标签中提供充分信息的原因）。它还会
	生成拉取请求的差异统计，以及拉取请求所含各个提交的简短日志。

Linus 回复说他倾向于使用 ``git://`` 协议。其他维护者可能有不同偏好。另请注意，
如果创建拉取请求时不使用签名标签，那么 ``https://`` 可能是更好的选择。完整讨论
请参阅原始邮件线程。


提交拉取请求
------------

拉取请求的提交方式与普通补丁相同。通过邮件正文将其发送给维护者，并视需要抄送
LKML 和相关子系统的邮件列表。发给 Linus 的拉取请求通常使用类似下面的主题行::

	[GIT PULL] <subsystem> changes for v4.15-rc1
