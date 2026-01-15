.. _jp_process_submitting_patches:

パッチの投稿: カーネルにコードを入れるための必須ガイド
======================================================

.. note::

   このドキュメントは :ref:`Documentation/process/submitting-patches.rst <submittingpatches>` の日本語訳です。

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

この文書は多数のセクションから構成されています。これらは比較的独立していますが、
順に読むことを推奨します。

.. _jp_submittingpatches_common_mistakes:

   * :ref:`投稿時によくある間違い <submittingpatches_common_mistakes>`
