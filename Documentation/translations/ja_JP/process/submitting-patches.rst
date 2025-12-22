.. _jp_process_submitting_patches:

パッチの投稿: カーネルにコードを入れるための必須ガイド
======================================================

.. note::

   このドキュメントは :ref:`Documentation/process/submitting-patches.rst <submittingpatches>` の日本語訳です。

   翻訳に関するご意見、誤植の指摘などは、Linuxカーネルドキュメント日本語翻訳プロジェクト
   <https://linux-kernel-docs-jp.osdn.jp/> へ連絡してください。

   免責事項: :ref:`translations_ja_JP_disclaimer`

.. warning::

   **UNDER CONSTRUCTION!!**

   この文書は翻訳更新の作業中です。最新の内容は原文を参照してください。

Linux カーネルに変更を加えたいと思っている個人や企業にとって、
その「仕組み」に慣れていなければ、投稿のプロセスは時に気後れするものでしょう。
この文書は、コードをカーネルに入れるための、主に技術的かつ手続き的な
手順の概要を説明することを目的としています。

もしこの文書を読んでいるあなたの目的が、単にバグ報告を送信することであれば、
Documentation/admin-guide/reporting-issues.rst
を参照してください。

この文書自体も長大ですが、詳細な手順書というわけではありません。
詳細については :ref:`Documentation/process/submit-checklist.rst <submitchecklist>`
を参照してください。
