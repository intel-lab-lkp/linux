.. SPDX-License-Identifier: GPL-2.0

IA Assistente de código
+++++++++++++++++++++++

Esta documentação fornece um guia para ferramentas de IA e desenvolvedores que
usam IA como assistente para contribuir para o kernel do Linux.

Ferramentas de IA que ajudam desenvolvedores do Linux kernel devem seguir os
padrões de desenvolvimento do kernel:

* Documentation/process/development-process.rst
* Documentation/process/coding-style.rst
* Documentation/process/submitting-patches.rst

Para guias e conteúdo sobre códigos gerados por assistentes de IA veja:

* Documentation/process/generated-content.rst

Licenças e Requisitos Legais
============================

Todas as contribuições devem estar de acordo com os requisitos de licença do
kernel:

* Todo código deve ser compatível apenas com GPL-2.0
* Use identificadores SPDX de licença apropriado
* Veja Documentation/process/license-rules.rst para mais detalhes

Assinatura e certificado de origem do desenvolvedor
===================================================

Agentes de IA NÃO DEVEM ser incluídos nas tags de assinaturas. Apenas pessoas
podem legalmente certificar o Certificado de Origem do Desenvolvedor (DCO).
A pessoa que envia é responsável por:

* Revisar todo código gerado por IA
* Garantir conformidade com os requisitos de licença
* Acrescentar sua própria tag de certificado de assinatura o DCO
* Assumir toda responsabilidade pela contribuição

Atribuições
===========

Quando ferramentas de IA contribuir com o desenvolvedor kernel, a atribuição
adequada ajuda a rastrear a função da IA no processo de desenvolvimento.
Contribuições devem incluir a tag de assistência seguindo este formato::

    Assisted-by: LLM [FERRAMENTA1] [FERRAMENTA2]

* ``[FERRAMENTA1] [FERRAMENTA2]`` são usadas ferramentas opcionais de análise
  especializadas (Exemplo: coccinelle, sparse, smatch, clang-tidy)

Ferramentas básicas de desenvolvimento (git, gcc, make, editors) não são
listadas.

Exemplo::

  Assisted-by: LLM coccinelle sparse