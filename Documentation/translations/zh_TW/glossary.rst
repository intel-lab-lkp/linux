.. SPDX-License-Identifier: GPL-2.0

術語表
======

這不是一個完善的術語表，我們只是將有爭議的和陌生的翻譯詞彙記錄於此，
它的篇幅應該根據內核文檔翻譯的需求而增加。新詞條最好隨翻譯補丁一起
提交，且僅在以下情況下收錄新詞條：

        - 在翻譯過程中遇到陌生詞彙，且尚無翻譯先例的；
        - 在審閱過程中，針對某詞條出現了不同的翻譯意見；
        - 使用頻率不高的詞條和首字母縮寫類型的詞條；
        - 已經存在且有歧義的詞條翻譯。


* atomic: 原子的，一般指不可中斷的極小的臨界區操作。
* DVFS: 動態電壓頻率升降。（Dynamic Voltage and Frequency Scaling）
* EAS: 能耗感知調度。（Energy Aware Scheduling）
* flush: 刷新，一般指對cache的沖洗操作。
* fork: 創建, 通常指父進程創建子進程。
* futex: 快速用戶互斥鎖。（fast user mutex）
* guest halt polling: 客戶機停機輪詢機制。
* HugePage: 巨頁。
* hypervisor: 虛擬機超級管理器。
* memory barriers: 內存屏障。
* MIPS: 每秒百萬指令。（Millions of Instructions Per Second）,注意與mips指令集區分開。
* mutex: 互斥鎖。
* NUMA: 非統一內存訪問。
* OpenCAPI: 開放相干加速器處理器接口。（Open Coherent Accelerator Processor Interface）
* OPP: 操作性能值。
* overhead: 開銷，一般指需要消耗的計算機資源。
* PELT: 實體負載跟蹤。（Per-Entity Load Tracking）
* sched domain: 調度域。
* semaphores: 信號量。
* spinlock: 自旋鎖。
* watermark: 水位，一般指頁表的消耗水平。
