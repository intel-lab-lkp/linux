.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/doc-guide/sphinx.rst

:译者:
 - 吴想成 Wu XiangCheng <bobwxc@email.cn>
 - 裘剑东 Jiandong Qiu <qiujiandong1998@gmail.com>

.. _sphinxdoc_zh:

简介
====

Linux内核使用 `Sphinx <http://www.sphinx-doc.org/>`_ 来把 ``Documentation``
下的 `reStructuredText <http://docutils.sourceforge.net/rst.html>`_ 文件转
换成漂亮的文档。使用 ``make htmldocs`` 或 ``make pdfdocs`` 命令即可构建HTML
或PDF格式的文档。生成的文档放在 ``Documentation/output`` 文件夹中。

reStructuredText文件可能包含来自源文件的结构化文档注释或kernel-doc注释。
通常它们用于描述代码的功能、类型和设计。kernel-doc注释有一些特殊的结构和
格式，但除此之外，它们还被作为reStructuredText处理。

最后，有成千上万的纯文本文档文件散布在 ``Documentation`` 里。随着时间推移，
其中一些可能会转换为reStructuredText，但其中大部分仍保持纯文本。

.. _sphinx_install_zh:

安装Sphinx
==========

Documentation/ 下的ReST文件现在使用 ``sphinx`` 3.4.3 或更高版本构建。

这有一个脚本可以检查Sphinx的依赖项。更多详细信息见
:ref:`sphinx-pre-install_zh` 。

大多数发行版都附带了Sphinx，但是它的工具链比较脆弱，而且在您的机器上升级它
或其他一些Python包导致文档构建中断的情况并不少见。

避免此情况的一种方法是使用与发行版附带的不同的版本。因此，建议使用
``virtualenv-3`` 或 ``virtualenv`` 在虚拟环境中安装Sphinx，具体取决于发行版
如何打包Python3。

总之，如您要安装最新版的Sphinx，应执行::

       $ virtualenv sphinx_latest
       $ . sphinx_latest/bin/activate
       (sphinx_latest) $ pip install -r Documentation/sphinx/requirements.txt

在运行 ``. sphinx_latest/bin/activate`` 之后，提示符将变化，以指示您正在使用新
环境。如果您打开了一个新的shell，那么在构建文档之前，您需要重新运行此命令以再
次进入虚拟环境中。

图片输出
--------

内核文档构建系统包含一个扩展，可以处理GraphViz和SVG格式的图像（参见
:ref:`sphinx_kfigure_zh` ）。

为了让它工作，您需要同时安装GraphViz和ImageMagick包。如果没有安装这些软件包，
构建系统仍将构建文档，但不会在输出中包含任何图像。

PDF和LaTeX构建
--------------

目前只有Sphinx 2.4及更高版本才支持这种构建。

对于PDF和LaTeX输出，还需要 ``XeLaTeX`` 3.14159265版本。（译注：此版本号真实
存在）

根据发行版的不同，您可能还需要安装一系列 ``texlive`` 软件包，这些软件包提供了
``XeLaTeX`` 工作所需的最小功能集。

HTML中的数学表达式
------------------

有些ReST页面中包含数学表达式。由于Sphinx的工作方式，这些表达式使用LaTeX
符号书写。Sphinx在HTML输出中渲染数学表达式有两种方式：一种是名为 `imgmath`_
的扩展，它将数学表达式转换成图像并嵌入到HTML页面中。
另一种是名为 `mathjax`_ 的扩展，它将数学表达式的渲染交由支持 JavaScript
的浏览器处理。
在6.1版内核文档之前，前者是唯一可用的方式，并且需要安装大量texlive软件包，
其中包括amsfonts和amsmath等。

自内核6.1版本起，无需安装任何texlive软件包即可构建包含数学表达式的HTML
页面。更多信息请参阅 :ref:`choice_of_math_renderer_zh`。

.. _imgmath: https://www.sphinx-doc.org/en/master/usage/extensions/math.html#module-sphinx.ext.imgmath
.. _mathjax: https://www.sphinx-doc.org/en/master/usage/extensions/math.html#module-sphinx.ext.mathjax

.. _sphinx-pre-install_zh:

检查Sphinx依赖项
----------------

这有一个脚本可以自动检查Sphinx依赖项。如果它认得您的发行版，还会提示您所用发行
版的安装命令::

	$ ./tools/docs/sphinx-pre-install
	Checking if the needed tools for Fedora release 26 (Twenty Six) are available
	Warning: better to also install "texlive-luatex85".
	You should run:

		sudo dnf install -y texlive-luatex85
		/usr/bin/virtualenv sphinx_2.4.4
		. sphinx_2.4.4/bin/activate
		pip install -r Documentation/sphinx/requirements.txt

	Can't build as 1 mandatory dependency is missing at ./tools/docs/sphinx-pre-install line 468.

默认情况下，它会检查html和PDF的所有依赖项，包括图像、数学表达式和LaTeX构建的
需求，并假设将使用虚拟Python环境。html构建所需的依赖项被认为是必需的，其他依
赖项则是可选的。

它支持两个可选参数：

``--no-pdf``

	禁用PDF检查；

``--no-virtualenv``

	使用系统发行版提供的Sphinx软件包，而不是Python虚拟环境。

安装最小版本的Sphinx
--------------------

在修改Sphinx构建系统时，确保其最小支持版本仍然可用非常重要。
然而，在现代发行版上，这样做正变得越来越困难，因为在Python 3.13及以上版本
环境下，已经无法安装该最小版本。

要按照 Documentation/translations/zh_CN/process/changes.rst
中定义的最低支持Python版本进行测试，可以使用该版本创建一个虚拟环境，
并安装最小依赖::

	/usr/bin/python3.9 -m venv sphinx_min
	. sphinx_min/bin/activate
	pip install -r Documentation/sphinx/min_requirements.txt

更全面的测试可以使用以下脚本完成:

	tools/docs/test_doc_build.py

该脚本会为每个受支持的Python版本创建一个虚拟环境，并可选择针对一定范围内的
Sphinx版本构建文档。

Sphinx构建
==========

生成文档的常用方法是运行 ``make htmldocs`` 或 ``make pdfdocs`` 。还有其它可用
的格式：请参阅 ``make help`` 的文档部分。生成的文档放在 ``Documentation/output``
下相应格式的子目录中。

要生成文档，显然必须安装Sphinx（ ``sphinx-build`` ）。对于PDF输出，您还需
要 ``XeLaTeX`` 和来自 ImageMagick（https://www.imagemagick.org）的
``convert(1)`` 。\ [#ink]_ 所有这些软件在各大发行版中都很常见，
也都有对应的软件包。

要传递额外的选项给Sphinx，可以使用make变量 ``SPHINXOPTS`` 。例如，使用
``make SPHINXOPTS=-v htmldocs`` 获得更详细的输出。

也可以通过make变量 ``DOCS_CSS`` 传入一个额外的DOCS_CSS覆盖文件，以自定义
html布局。

默认情况下，构建HTML文档时使用的是"Alabaster"主题；该主题随Sphinx一同提供，
无需单独安装。也可以通过make变量 ``DOCS_THEME`` 覆盖Sphinx主题。

.. note::

   有些人可能更喜欢在html输出中使用RTD主题。根据Sphinx版本的不同，它可能需要
   单独安装，可使用 ``pip install sphinx_rtd_theme`` 进行安装。

还有一个make变量 ``SPHINXDIRS`` ，在测试构建某个文档子集时很有用。例如，您可
以通过运行 ``make SPHINXDIRS=doc-guide htmldocs`` 构建
``Documentation/doc-guide`` 下的文档。 ``make help`` 的文档部分会显示可指定的
子目录列表。

要删除生成的文档，请运行 ``make cleandocs`` 。

.. [#ink] 如果还安装了来自 Inkscape（https://inkscape.org）的 ``inkscape(1)`` ，
	  则能够提升嵌入到PDF文档中的图像质量，尤其是在5.18及之后的内核版本中。

.. _choice_of_math_renderer_zh:

数学渲染器的选择
----------------

自内核 6.1 版本起， ``mathjax`` 可作为html输出中数学渲染器的后备方案。\ [#sph1_8_zh]_

数学渲染器会根据可用命令按如下方式选择：

.. table:: HTML 中数学渲染器的选择

    ============ ================= ============
    数学渲染器   所需命令          图像格式
    ============ ================= ============
    imgmath      latex, dvipng     PNG（栅格）
    mathjax
    ============ ================= ============

也可以通过设置环境变量 ``SPHINX_IMGMATH`` 来覆盖该选择，如下所示：

.. table:: 设置 ``SPHINX_IMGMATH`` 的效果

    ====================== ==========
    设置                   渲染器
    ====================== ==========
    ``SPHINX_IMGMATH=yes`` imgmath
    ``SPHINX_IMGMATH=no``  mathjax
    ====================== ==========

.. [#sph1_8_zh] 数学渲染器的后备机制要求Sphinx >= 1.8。

编写文档
========

添加新文档很容易，只需：

1. 在 ``Documentation`` 下某处添加一个新的 ``.rst`` 文件。
2. 从 ``Documentation/index.rst`` 中的Sphinx `主目录树`_ 链接到它。

.. _主目录树: http://www.sphinx-doc.org/en/stable/markup/toctree.html

对于简单的文档（比如您现在正在阅读的文档），这通常已经足够好了，但是对于较大
的文档，最好创建一个子目录（或者使用现有的子目录）。例如，图形子系统文档位于
``Documentation/gpu`` 下，拆分为多个 ``.rst`` 文件，并具有从主目录链接来的单
独索引 ``index.rst`` （有自己的目录树 ``toctree`` ）。

请参阅 `Sphinx <http://www.sphinx-doc.org/>`_ 和 `reStructuredText
<http://docutils.sourceforge.net/rst.html>`_ 的文档，以了解如何使用它们。
特别是Sphinx `reStructuredText 基础`_ 这是开始学习使用reStructuredText的
好地方。还有一些 `Sphinx 特殊标记结构`_ 。

.. _reStructuredText 基础: http://www.sphinx-doc.org/en/stable/rest.html
.. _Sphinx 特殊标记结构: http://www.sphinx-doc.org/en/stable/markup/index.html

内核文档的具体指南
------------------

这是一些内核文档的具体指南：

* 请不要过于痴迷转换格式到reStructuredText。保持简单。在大多数情况下，文档
  应该是纯文本，格式应足够一致，以便可以转换为其他格式。

* 将现有文档转换为reStructuredText时，请尽量减少格式更改。

* 在转换文档时，还要更新内容，而不仅仅是格式。

* 请遵循标题修饰符的顺序：

  1. ``=`` 文档标题，要有上线::

       ========
       文档标题
       ========

  2. ``=`` 章::

       章标题
       ======

  3. ``-`` 节::

       节标题
       ------

  4. ``~`` 小节::

       小节标题
       ~~~~~~~~

  尽管RST没有规定具体的顺序（“没有强加一个固定数量和顺序的节标题装饰风格，最终
  按照的顺序将是实际遇到的顺序。”），但整体上让较高层级的标题样式保持一致，
  会更便于阅读和理解文档结构。

* 对于插入固定宽度的文本块（用于代码样例、用例等）： ``::`` 用于语法高亮意义不
  大的内容，尤其是短代码段； ``.. code-block:: <language>`` 用于需要语法高亮的
  较长代码块。对于嵌入到文本中的简短代码片段，请使用 \`\` 。


C域
---

**Sphinx C域（Domain）** （name c）适用于C API文档。例如，函数原型：

.. code-block:: rst

    .. c:function:: int ioctl( int fd, int request )

内核文档的C域有一些附加特性。例如，您可以使用诸如 ``open`` 或 ``ioctl`` 这样的
通用名称重命名函数的引用名称：

.. code-block:: rst

     .. c:function:: int ioctl( int fd, int request )
        :name: VIDIOC_LOG_STATUS

函数名称（例如ioctl）仍保留在输出中，但引用名称从 ``ioctl`` 变为
``VIDIOC_LOG_STATUS`` 。此函数的索引项也变为 ``VIDIOC_LOG_STATUS`` 。

请注意，不需要使用 ``c:func:`` 生成函数文档的交叉引用。由于一些Sphinx扩展的
神奇力量，如果给定函数名的索引项存在，文档构建系统会自动将对 ``function()``
的引用转换为交叉引用。如果在内核文档中看到 ``c:func:`` 的用法，请删除它。

表格
----

reStructuredText 为表格语法提供了多种选项。内核文档中的表格通常优先使用
*simple table* 或 *grid table* 语法。更多细节请参阅
`reStructuredText 用户参考中的表格语法`_ 。

.. _reStructuredText 用户参考中的表格语法:
   https://docutils.sourceforge.io/docs/user/rst/quickref.html#tables

列式表
~~~~~~

列式表（``list-table``）格式对于无法用常规Sphinx ASCII艺术格式轻松排版的表格来说
可能很有用。然而，这种格式对于纯文本文档的读者来说几乎无法理解，因此，
除非有充分的理由，否则应避免使用。

``flat-table`` 也是一个二级列表，类似于 ``list-table`` ，但具有一些额外特性：

* 列范围：使用 ``cspan`` 修饰，可以通过其他列扩展单元格

* 行范围：使用 ``rspan`` 修饰，可以通过其他行扩展单元格

* 自动将表格行最右边的单元格扩展到该行右侧空缺的单元格上。若使用
  ``:fill-cells:`` 选项，此行为可以从 *自动合并* 更改为 *自动插入* ，自动
  插入（空）单元格，而不是扩展合并到最后一个单元格。

选项：

* ``:header-rows:``   [int] 标题行计数
* ``:stub-columns:``  [int] 标题列计数
* ``:widths:``        [[int] [int] ... ] 列宽
* ``:fill-cells:``    插入缺少的单元格，而不是自动合并缺少的单元格

修饰：

* ``:cspan:`` [int] 扩展列
* ``:rspan:`` [int] 扩展行

下面的例子演示了如何使用这些标记。分级列表的第一级是 *表格行* 。 *表格行* 中
只允许一个标记，即该 *表格行* 中的单元格列表。 *comments* （ ``..`` ）和
*targets* 例外（例如引用 ``:ref:`最后一行 <last row_zh>``` / :ref:`最后一行
<last row_zh>` ）。

.. code-block:: rst

   .. flat-table:: 表格标题
      :widths: 2 1 1 3

      * - 表头 列1
        - 表头 列2
        - 表头 列3
        - 表头 列4

      * - 行1
        - 字段1.1
        - 字段1.2（自动扩展）

      * - 行2
        - 字段2.1
        - :rspan:`1` :cspan:`1` 字段2.2~3.3

      * .. _`last row_zh`:

        - 行3

渲染效果：

   .. flat-table:: 表格标题
      :widths: 2 1 1 3

      * - 表头 列1
        - 表头 列2
        - 表头 列3
        - 表头 列4

      * - 行1
        - 字段1.1
        - 字段1.2（自动扩展）

      * - 行2
        - 字段2.1
        - :rspan:`1` :cspan:`1` 字段2.2~3.3

      * .. _`last row_zh`:

        - 行3

交叉引用
--------

从一页文档到另一页文档的交叉引用可以通过简单地写出文件路径来完成，无特殊格式
要求。路径可以是绝对路径或相对路径。绝对路径从“Documentation/”开始。例如，要
交叉引用此页，以下写法皆可，取决于具体的文档目录（注意 ``.rst`` 扩展名是必需
的）::

    参见 Documentation/doc-guide/sphinx.rst 。此法始终可用。
    请查看 sphinx.rst ，仅在同级目录中有效。
    请阅读 ../sphinx.rst ，上级目录中的文件。

如果希望链接显示的文本不同于文档标题，则需要使用 Sphinx 的 ``doc`` 修饰。例
如::

    参见 :doc:`sphinx文档的自定义链接文本 <sphinx>`.

对于大多数用例，前者是首选，因为它更干净，更适合阅读源文件的人。如果您遇到一
个没有任何特殊作用的 ``:doc:`` 用法，请将其转换为文档路径。

有关交叉引用kernel-doc函数或类型的信息，请参阅
Documentation/translations/zh_CN/doc-guide/kernel-doc.rst 。

引用提交
~~~~~~~~

对 git 提交的引用如果采用以下格式书写，会自动转换为超链接::

    commit 72bf4f1767f0
    commit 72bf4f1767f0 ("net: do not leave an empty skb in write queue")

.. _sphinx_kfigure_zh:

图形图片
========

如果要添加图片，应该使用 ``kernel-figure`` 和 ``kernel-image`` 指令。例如，
要插入具有可缩放图像格式的图形，请使用SVG（:ref:`svg_image_example_zh` ）::

    .. kernel-figure::  ../../../doc-guide/svg_image.svg
       :alt:    简易 SVG 图片

       SVG 图片示例

.. _svg_image_example_zh:

.. kernel-figure::  ../../../doc-guide/svg_image.svg
   :alt:    简易 SVG 图片

   SVG 图片示例

内核figure（和image）指令支持 DOT 格式文件，请参阅

* DOT：http://graphviz.org/pdf/dotguide.pdf
* Graphviz：http://www.graphviz.org/content/dot-language

一个简单的例子（:ref:`hello_dot_file_zh` ）::

  .. kernel-figure::  ../../../doc-guide/hello.dot
     :alt:    你好，世界

     DOT 示例

.. _hello_dot_file_zh:

.. kernel-figure::  ../../../doc-guide/hello.dot
   :alt:    你好，世界

   DOT 示例

嵌入的渲染标记（或语言），如Graphviz的 **DOT** 由 ``kernel-render`` 指令提供::

  .. kernel-render:: DOT
     :alt: 有向图
     :caption: 嵌入式 **DOT** (Graphviz) 代码

     digraph foo {
      "五棵松" -> "国贸";
     }

如何渲染取决于安装的工具。如果安装了Graphviz，您将看到一个矢量图像。否则，原始
标记将作为 *文字块* 插入（:ref:`hello_dot_render_zh` ）。

.. _hello_dot_render_zh:

.. kernel-render:: DOT
   :alt: 有向图
   :caption: 嵌入式 **DOT** (Graphviz) 代码

   digraph foo {
      "五棵松" -> "国贸";
   }

*render* 指令包含 *figure* 指令中已知的所有选项，以及选项 ``caption`` 。如果
``caption`` 有值，则插入一个 *figure* 节点，若无，则插入一个 *image* 节点。
如果您想引用它，还需要一个 ``caption`` （:ref:`hello_svg_render_zh` ）。

嵌入式 **SVG**::

  .. kernel-render:: SVG
     :caption: 嵌入式 **SVG** 标记
     :alt: 右上箭头

     <?xml version="1.0" encoding="UTF-8"?>
     <svg xmlns="http://www.w3.org/2000/svg" version="1.1" ...>
        ...
     </svg>

.. _hello_svg_render_zh:

.. kernel-render:: SVG
   :caption: 嵌入式 **SVG** 标记
   :alt: 右上箭头

   <?xml version="1.0" encoding="UTF-8"?>
   <svg xmlns="http://www.w3.org/2000/svg"
     version="1.1" baseProfile="full" width="70px" height="40px" viewBox="0 0 700 400">
   <line x1="180" y1="370" x2="500" y2="50" stroke="black" stroke-width="15px"/>
   <polygon points="585 0 525 25 585 50" transform="rotate(135 525 25)"/>
   </svg>

