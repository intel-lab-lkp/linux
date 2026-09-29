.. SPDX-License-Identifier: GPL-2.0

.. include:: ../../disclaimer-zh_TW.rst

:Original: Documentation/process/debugging/index.rst

============================
給Linux核心開發者的除錯建議
============================

一般指南
--------

.. toctree::
   :maxdepth: 1

   gdb-kernel-debugging

TODOList:

* driver_development_debugging_guide
* kgdb
* userspace_debugging_guide

特定子系統指南
--------------

TODOList:

* media_specific_debugging_guide

一般除錯建議
============

視問題而定，可用來追蹤問題、甚至用來確認問題是否真的存在的工具也各不相同。

第一步，你必須先弄清楚要除錯的是哪一類問題。根據答案的不同，你的方法與
工具選擇也可能有所不同。

我是否需要在存取受限的情況下除錯？
----------------------------------

你對機器的存取是否受到限制，或是無法中斷正在進行的執行？

在這種情況下，你的除錯能力取決於發行版所提供的核心內建的除錯支援。
Documentation/process/debugging/userspace_debugging_guide.rst 簡要介紹了在
這種情況下可用的各種除錯工具。在大多數情況下，你可以查看/boot目錄中的
設定檔，來確認你的核心具備哪些功能。

我是否擁有系統的root存取權限？
------------------------------

你能否輕易地替換有問題的模組，或安裝新的核心？

若是如此，你可用的工具就多得多，相關工具請參見
Documentation/process/debugging/driver_development_debugging_guide.rst 。

時序是否是影響因素？
--------------------

重要的是弄清楚你要除錯的問題是穩定重現（也就是給定一組輸入時，總是得到
相同的錯誤輸出），還是時有時無。如果問題時有時無，可能有某種時序因素在
作用。如果在程式碼中插入延遲確實會改變其行為，那麼時序很可能就是其中一個
因素。

當時序確實會改變程式碼的執行結果時，用簡單的printk()來除錯可能行不通；
類似的替代方案是使用trace_printk()，它會把除錯訊息記錄到追蹤檔案，而不是
核心日誌。

**Copyright** ©2024 : Collabora
