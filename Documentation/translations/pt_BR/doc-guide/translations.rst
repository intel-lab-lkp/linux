.. SPDX-License-Identifier: GPL-2.0

===================================
Traduzindo a documentação do kernel
===================================

A documentação do kernel é escrita em inglês, e o inglês é a única versão
oficial dela. As traduções existem para tornar essa documentação acessível
a quem se sente mais à vontade lendo em outro idioma; elas ficam em
``Documentation/translations/``, com um diretório por idioma.

Este documento explica como contribuir com essas traduções: como escolher
o que traduzir, como um documento traduzido é organizado, como verificar
o seu trabalho, como enviá-lo, o que esperar da revisão e como manter uma
tradução atualizada depois que ela for integrada. Ele também descreve o
que é necessário para iniciar a tradução para um novo idioma.

Ele não substitui os documentos gerais sobre como contribuir com o kernel;
um patch de tradução é um patch do kernel, e tudo o que está descrito em
:doc:`../process/submitting-patches` se aplica a ele. O que segue se
concentra nos detalhes específicos das traduções e nos erros que mais
frequentemente fazem com que patches de tradução sejam devolvidos.

Algumas traduções têm orientações adicionais, específicas do idioma e
escritas nele; por exemplo, a equipe do chinês simplificado mantém
Documentation/translations/zh_CN/how-to.rst. Onde existir um guia assim,
leia-o também.


Princípios
==========

Antes de começar, leia o aviso em
:ref:`Documentation/translations/index.rst <translations_disclaimer>`.
Seus pontos principais orientam todo o restante deste documento:

- **O inglês é a referência.** Quando uma tradução e o original em inglês
  divergem, a versão em inglês é a correta.

- **Uma tradução não é um fork.** Ela não deve acrescentar, remover nem
  alterar informações. Se você encontrar um erro, uma omissão ou uma
  afirmação desatualizada no original, corrija primeiro o documento em
  inglês, com um patch separado enviado aos mantenedores da documentação.
  Depois que essa alteração for integrada, a tradução pode acompanhá-la.

- **Os mantenedores das traduções só aceitam trabalho de tradução**: novas
  traduções, atualizações que acompanham mudanças no original e correções
  em traduções existentes.

- **Adaptar é permitido; mudar o sentido, não.** Cada idioma tem sua
  própria gramática e suas expressões, e uma tradução literal, palavra por
  palavra, muitas vezes fica errada. Reformule o quanto o seu idioma
  exigir, desde que o leitor termine com a mesma informação que um leitor
  do original.


Antes de começar
================

Encontre a equipe da tradução
-----------------------------

Cada tradução está listada no arquivo MAINTAINERS, com uma linha ``F:``
apontando para o seu diretório. Você pode encontrar os responsáveis por
uma tradução com::

  $ scripts/get_maintainer.pl -f Documentation/translations/<lang>/

Acompanhe as discussões recentes na lista de discussão linux-doc
(https://lore.kernel.org/linux-doc/) para ter uma ideia de como a equipe
trabalha, quais convenções segue e o que os revisores costumam pedir. Se
estiver em dúvida sobre algo, pergunte na lista antes de investir muito
trabalho; os mantenedores das traduções costumam ficar contentes em ajudar
quem está começando.

Escolha o que traduzir
----------------------

Bons candidatos são os documentos que muita gente lê: os documentos sobre
o processo de desenvolvimento em ``Documentation/process/``, o guia do
administrador e os guias sobre como escrever documentação. Documentos
longos, que mudam com frequência ou muito especializados são mais difíceis
de manter atualizados e é melhor deixá-los para depois.

Verifique se o documento ainda não foi traduzido e se ninguém mais está
trabalhando nele:

- procure em ``Documentation/translations/<lang>/``;

- pesquise o nome do arquivo no arquivo da linux-doc em lore.kernel.org;
  um patch pendente, ou uma revisão recente, significa que alguém já está
  cuidando dele;

- use o ``tools/docs/checktransupdate.py`` para listar os documentos que
  ainda não têm tradução::

    $ tools/docs/checktransupdate.py -l <lang> --print-missing-translations

  Veja Documentation/doc-guide/checktransupdate.rst para mais detalhes.

Evite que várias pessoas traduzam arquivos do mesmo diretório ao mesmo
tempo. Nesse caso, os patches tendem a alterar o mesmo ``index.rst`` e
acabam entrando em conflito entre si. Se você pretende traduzir um
diretório inteiro, avise na lista.

Se você é novo no desenvolvimento do kernel, comece com um documento
curto. É no primeiro patch que aparece a maioria dos problemas de
processo, e eles são muito mais fáceis de corrigir em um arquivo de 60
linhas do que em um de 2000.

Prepare o seu ambiente
----------------------

As traduções são integradas pela árvore da documentação, então baseie o
seu trabalho no branch ``docs-next`` dela (a linha ``T:`` da entrada
DOCUMENTATION no MAINTAINERS indica onde ela fica)::

  $ git remote add docs git://git.lwn.net/linux.git
  $ git fetch docs
  $ git switch -c minha-traducao docs/docs-next

Algumas traduções são reunidas primeiro em uma árvore própria; se a
entrada do MAINTAINERS do seu idioma tiver a sua própria linha ``T:``,
pergunte aos mantenedores da tradução qual branch usar.

Você vai precisar de uma instalação do Sphinx funcionando para compilar a
documentação. O ``tools/docs/sphinx-pre-install`` verifica o seu sistema e
informa o que está faltando; Documentation/doc-guide/sphinx.rst traz os
detalhes.

Você também vai precisar conseguir enviar e-mails em texto puro com o
``git send-email`` ou com um cliente de e-mail configurado como descrito
em :doc:`../process/email-clients`. As listas de discussão do
vger.kernel.org rejeitam e-mails em HTML, então respostas enviadas por uma
interface de webmail na configuração padrão nunca chegam à lista.


Anatomia de um documento traduzido
==================================

Localização e nome do arquivo
-----------------------------

Uma tradução reproduz o caminho do seu original dentro do diretório do
idioma e mantém o mesmo nome de arquivo::

  Documentation/process/howto.rst
  Documentation/translations/<lang>/process/howto.rst

Não traduza nomes de arquivos nem de diretórios. A compilação da
documentação usa essa correspondência para adicionar, no topo de cada
página, links para a mesma página nos outros idiomas; isso só funciona se
os caminhos coincidirem.

O cabeçalho do arquivo
----------------------

Comece o arquivo com o mesmo identificador de licença SPDX do original. A
maioria das traduções indica em seguida, de alguma forma, qual documento
foi traduzido e inclui o aviso do idioma. Por exemplo, a tradução para o
italiano usa::

  .. include:: ../disclaimer-ita.rst

  :Original: Documentation/core-api/symbol-namespaces.rst

Um caminho simples basta no campo ``:Original:``: a compilação da
documentação transforma automaticamente os caminhos ``Documentation/...``
em links.

O cabeçalho exato varia de um idioma para outro: alguns incluem o aviso em
todos os arquivos, outros apenas na página de índice, e alguns listam os
tradutores enquanto outros deixam as informações de contato para o
MAINTAINERS. Copie o cabeçalho de um documento integrado recentemente no
seu idioma, em vez de inventar o seu, e ajuste o caminho relativo de
qualquer diretiva ``include::`` à profundidade do seu arquivo.

Se o seu idioma lista os tradutores em cada arquivo, evite duplicar
informações que já estão no MAINTAINERS. Endereços de e-mail em documentos
ficam desatualizados; o MAINTAINERS é o lugar onde eles são mantidos em
dia.

Rótulos
-------

Os rótulos do Sphinx (``.. _rotulo:``) são globais para toda a compilação
da documentação. Um rótulo copiado sem alteração do original, portanto,
define o mesmo nome duas vezes, o que quebra a compilação ou, na melhor
das hipóteses, faz as referências apontarem para o documento errado.

- Os rótulos colocados bem no início de um arquivo normalmente não são
  necessários na tradução; basta removê-los.

- Os rótulos necessários, por exemplo porque outras partes do documento
  traduzido se referem a eles, devem se tornar únicos com o acréscimo de
  um prefixo do idioma. Cada tradução já usa um; veja os arquivos
  existentes para descobrir qual (``it_``, ``sp_``, ``cn_``, ``tw_``,
  ``pt_``, ...)::

    .. _it_process_howto:

- Referências a rótulos da documentação em inglês são aceitáveis e
  esperadas. Quando o documento de destino ainda não foi traduzido, criar
  o link para a versão em inglês é a coisa certa a fazer.

Citações e outros alvos com nome
--------------------------------

As citações com nome também são globais. Se o original contém::

  ... the C programming language [c-language]_ ...

  .. [c-language] http://www.open-std.org/jtc1/sc22/wg14/www/standards

então uma tradução que mantém o nome ``c-language`` gera um aviso de
"duplicate citation" para cada citação do arquivo. Acrescente o prefixo do
idioma tanto às definições quanto aos seus usos::

  ... a linguagem de programação C [pt-c-language]_ ...

  .. [pt-c-language] http://www.open-std.org/jtc1/sc22/wg14/www/standards

O mesmo vale para qualquer outra construção que defina um nome visível
fora do arquivo.

Referências cruzadas
--------------------

Mantenha todas as referências cruzadas do original e decida, para cada
uma, para onde ela deve apontar:

- se o documento de destino já estiver traduzido para o seu idioma, aponte
  para a tradução, para que os leitores continuem no seu idioma;

- caso contrário, mantenha a referência ao documento em inglês.

Dentro do diretório da tradução, o ``:doc:`` aceita caminhos relativos ao
arquivo atual; assim, um documento em
``Documentation/translations/<lang>/process/`` pode se referir ao
``howto.rst`` traduzido com ``:doc:`howto```. Caminhos absolutos, como
``:doc:`/process/howto```, sempre apontam para a documentação em inglês.

Traduza o texto visível de uma referência quando for adequado, mas nunca o
seu destino::

  :ref:`Criando um tracepoint <process/debugging/driver_development_debugging_guide:ftrace>`

O que não deve ser traduzido
----------------------------

Alguns conteúdos precisam permanecer exatamente como estão no original:

- blocos literais (``::``), diretivas ``code-block``, linhas de comando e
  suas saídas, mensagens de log e exemplos de código; eles são copiados
  sem alteração, incluindo a indentação e os caracteres de tabulação;

- identificadores: nomes de funções, estruturas, variáveis e macros,
  opções do Kconfig, nomes e caminhos de arquivos, nomes de comandos e
  suas opções, entradas do sysfs e do procfs;

- os destinos das referências, as URLs e os argumentos de diretivas como
  ``kernel-doc``, ``include`` ou ``toctree``;

- os nomes das tags e outros textos fixos que as pessoas precisam digitar,
  como ``Signed-off-by:`` ou ``Reviewed-by:``.

O texto ao redor desses elementos é traduzido normalmente. Quando uma
frase cita um identificador, mantenha o identificador e a sua marcação e
traduza o restante da frase.

Estrutura e formatação do documento
-----------------------------------

Mantenha a estrutura do original: as mesmas seções, na mesma ordem, com os
mesmos níveis de título e, de preferência, os mesmos parágrafos. Isso
torna a tradução muito mais fácil de revisar, e muito mais fácil de
atualizar depois, porque uma mudança no original corresponde a um lugar
óbvio na tradução.

Siga as regras habituais da documentação do kernel:

- quebre as linhas em 80 colunas; blocos literais, tabelas e URLs longas
  são as exceções habituais;

- faça o sublinhado (e o sobrelinhado) dos títulos pelo menos tão longo
  quanto o texto do título, levando em conta que os caracteres do leste
  asiático ocupam duas colunas;

- use UTF-8, sem marca de ordem de bytes (BOM) e sem espaços em branco no
  fim das linhas;

- mantenha a marcação reStructuredText válida. A marcação inline, como a
  ênfase ou os literais inline, precisa estar separada do texto ao redor
  por espaços ou pontuação, e a ordem das palavras de uma tradução pode
  mudar o que fica ao redor dela. Quando a marcação precisar ser seguida
  diretamente por texto, separe-os com um espaço escapado (uma barra
  invertida seguida de um espaço), que não aparece na saída::

    ``vmstat``\ *, mas por processo*

Se algo realmente precisar ser explicado aos leitores da tradução, como um
termo que não tem um equivalente estabelecido, marque-o claramente como
uma nota do tradutor (``.. note::``) em vez de misturá-lo ao texto
traduzido. Use essas notas com moderação; elas não são uma forma de
acrescentar conteúdo.


Qualidade da tradução
=====================

Fidelidade
----------

Compare a sua tradução com o original, parágrafo por parágrafo, antes de
enviá-la. Procure por:

- frases ou itens de lista que foram omitidos ou acrescentados;

- afirmações cujo sentido mudou, por exemplo um pronome relativo que,
  depois da tradução, passa a se referir a outro substantivo, ou um
  "should" que virou um "must";

- números, versões, nomes de opções e caminhos que mudaram ao serem
  redigitados.

Terminologia
------------

Use os mesmos termos que o restante da tradução para o seu idioma. Antes
de escolher uma tradução para um termo técnico, pesquise nos documentos
existentes::

  $ git grep -i "<termo>" Documentation/translations/<lang>/

Algumas equipes mantêm um glossário (por exemplo,
Documentation/translations/zh_CN/glossary.rst); siga-o, se houver um.
Muitos termos em inglês (kernel, patch, commit, driver, ...) costumam ser
usados como estão por desenvolvedores que falam outros idiomas, e a
tradução deve seguir o uso estabelecido pela comunidade em vez de impor um
equivalente literal.

Se você discorda de um termo estabelecido, proponha a mudança
separadamente, para toda a tradução, em vez de introduzir um segundo termo
em um documento novo.

Idioma
------

Escreva um texto natural e gramaticalmente correto no seu idioma. Um
revisor que precisa corrigir concordância, pontuação ou ordem das
palavras em cada parágrafo não consegue prestar atenção ao conteúdo. Ler a
página HTML gerada, em vez do arquivo-fonte, é uma boa forma de perceber
frases estranhas.

Tradução automática e ferramentas de IA
---------------------------------------

Você é responsável por cada palavra que envia. Se usar tradução automática
ou um assistente de IA, revise e corrija o resultado com o mesmo cuidado
que teria se o tivesse escrito; traduções enviadas sem esse trabalho
dificilmente são aceitas, e os revisores podem perguntar como uma tradução
foi produzida.

As regras de :doc:`../process/coding-assistants` valem para as traduções
como para qualquer outra contribuição; em particular, a tag
``Signed-off-by:`` só pode ser adicionada pela pessoa que certifica a
contribuição.


Adicionando o documento à compilação
====================================

Um documento novo precisa estar listado em um ``toctree``; caso contrário,
o Sphinx avisa que ele "isn't included in any toctree" e os leitores não
conseguem encontrá-lo.

Adicione-o ao ``index.rst`` do diretório no seu idioma, na mesma posição
que ele ocupa no ``index.rst`` em inglês. Se o diretório ainda não existir
na sua tradução, crie-o com um ``index.rst`` próprio e crie o link para
esse índice a partir do índice do diretório acima.

Algumas traduções mantêm, nos seus arquivos de índice, uma lista dos
documentos que ainda não foram traduzidos. Se a sua mantiver, remova o
documento dessa lista no mesmo patch.


Compilando e verificando o seu trabalho
=======================================

Compile a documentação
----------------------

Compile a documentação HTML completa e verifique que a sua mudança não
introduz nenhum aviso novo::

  $ make htmldocs

Compilar apenas um subdiretório com ``SPHINXDIRS`` é mais rápido, e não há
problema em fazer isso enquanto você trabalha::

  $ make SPHINXDIRS=translations/<lang> htmldocs

mas não é suficiente antes do envio. Uma compilação parcial como essa não
contém os documentos em inglês; por isso, não consegue detectar rótulos ou
citações duplicados e informa como desconhecida toda referência a um
documento em inglês, o que esconde os problemas reais no meio do ruído.

A compilação completa da árvore já gera alguns avisos que não têm relação
com a sua mudança. A forma mais simples de verificar que você não
acrescentou nenhum é comparar os avisos antes e depois do seu patch::

  $ make cleandocs && make htmldocs 2>&1 | tee ../antes.log
  $ git am minha-traducao.patch      # ou mude para o seu branch
  $ make cleandocs && make htmldocs 2>&1 | tee ../depois.log
  $ diff <(grep -E 'WARNING|ERROR' ../antes.log | sort) \
         <(grep -E 'WARNING|ERROR' ../depois.log | sort)

O ``make cleandocs`` é importante: o Sphinx só recompila os arquivos que
mudaram, então uma compilação incremental não repete os avisos dos
arquivos que ela ignorou.

Confira o resultado
-------------------

Abra a página gerada, em ``Documentation/output/``, em um navegador.
Verifique se títulos, listas, tabelas, blocos literais e links aparecem
como esperado, e se os links de idioma no topo da página levam ao original
e às outras traduções do mesmo documento.

Para idiomas que precisam de fontes especiais, também vale a pena gerar a
saída em PDF (``make pdfdocs``) de vez em quando.

Rode o checkpatch
-----------------

Rode o ``scripts/checkpatch.pl`` no seu patch e corrija o que ele
apontar::

  $ scripts/checkpatch.pl --strict -g HEAD

Os avisos mais comuns em patches de tradução são:

- a falta do identificador de licença SPDX;

- linhas mais longas que o limite;

- a falta da descrição do commit;

- "added, moved or deleted file(s), does MAINTAINERS need updating?";
  este pode ser ignorado quando o diretório da tradução já é coberto por
  uma entrada do MAINTAINERS, o que acontece com todas as traduções
  existentes.


Preparando o patch
==================

Escopo
------

Envie um documento por patch. Vários documentos relacionados podem ser
enviados como uma série de patches, com um patch por documento.

Nunca misture alterações na documentação em inglês e em uma tradução no
mesmo patch. Elas são revisadas por pessoas diferentes, e a alteração em
inglês pode precisar de várias revisões antes que a tradução possa
acompanhá-la.

Linha de assunto
----------------

Use o prefixo que a sua tradução já usa; ``git log --oneline --
Documentation/translations/<lang>/`` o mostra. Exemplos de prefixos em uso
atualmente são ``docs/zh_CN:``, ``docs/zh_TW:``, ``doc:it_IT:`` e
``docs: pt_BR:``. Em seguida, descreva a mudança de forma breve::

  docs: pt_BR: translate process/programming-language.rst

Mensagem de commit
------------------

Escreva a mensagem de commit em inglês, para que todos os envolvidos na
integração do patch possam lê-la. Diga qual documento foi traduzido ou
atualizado e em qual estado do original a tradução se baseia. O script
``checktransupdate.py`` reconhece a frase abaixo, seguida do hash
abreviado (12 caracteres) e do assunto do último commit que alterou o
original::

  Update the translation through commit 1234567890ab
  ("docs: some subject of the last change to the original")

Com ela, o script consegue dizer exatamente quais mudanças do original
ainda faltam na tradução, mesmo que a tradução tenha sido escrita muito
depois desse commit. O hash e o assunto são mostrados por::

  $ git log -1 --format='%h ("%s")' --abbrev=12 -- Documentation/<caminho>.rst

Tags
----

As tags de um patch ficam em um único bloco no fim da mensagem de commit,
sem linhas em branco entre elas. O git só interpreta o último parágrafo
da mensagem como trailers, então uma linha em branco no meio faz com que
ele ignore as tags acima dela::

  Reviewed-by: Um Revisor <revisor@example.org>
  Signed-off-by: Seu Nome <voce@example.org>

O ``Signed-off-by:`` deve trazer o seu nome real; veja "Sign your work -
the Developer's Certificate of Origin" em
:doc:`../process/submitting-patches`.

Quando você enviar uma nova versão de um patch, mantenha as tags
``Reviewed-by:`` e ``Acked-by:`` que recebeu, a menos que o patch tenha
mudado significativamente desde que elas foram dadas.


Enviando o patch
================

Destinatários
-------------

Rode o ``scripts/get_maintainer.pl`` no patch para obter a lista de
destinatários::

  $ scripts/get_maintainer.pl 0001-docs-pt_BR-translate-something.patch

Para uma tradução, isso normalmente inclui os mantenedores da tradução, o
mantenedor da documentação e a lista de discussão linux-doc. Envie o patch
para todos eles; deixar de fora o mantenedor da documentação, que aplica a
maioria dos patches de tradução, é uma forma comum de ter um patch
ignorado.

Enviando
--------

Envie o patch com o ``git send-email`` ou com um cliente de e-mail que não
o altere. É uma boa ideia enviá-lo primeiro para você mesmo e verificar se
ele se aplica sem problemas com o ``git am`` sobre o branch que você usou
como base.

Mencione abaixo da linha ``---`` tudo o que os revisores devem saber, mas
que não deve entrar no histórico do git, como dúvidas sobre termos dos
quais você não tinha certeza.

Novas versões
-------------

Os revisores muitas vezes pedem mudanças. Responda aos comentários deles
na lista, intercalando as respostas no texto e em texto puro, e então
envie uma nova versão do patch:

- identifique-a como tal no assunto, por exemplo ``[PATCH v2]``;

- descreva o que mudou desde a versão anterior abaixo da linha ``---`` e
  acrescente links para as versões anteriores em lore.kernel.org::

    ---
    Changes in v2:
    - Use "namespace de usuário", as in the other pt_BR documents.
    - Prefix the citation names to avoid duplicates.

    v1: https://lore.kernel.org/linux-doc/<message-id>/

- trate todos os comentários, seja alterando o patch, seja explicando por
  que não o fez;

- não tenha pressa; dê aos revisores mais ou menos um dia para comentar
  uma versão antes de enviar a próxima.

Não acrescente ``RESEND`` ao assunto de um patch modificado; essa palavra
é reservada para reenviar sem alterações um patch que não recebeu
resposta, como descrito em :doc:`../process/submitting-patches`.


O processo de revisão
=====================

Os patches de tradução são revisados pelos mantenedores da tradução e por
outros falantes do idioma, e normalmente são aplicados pelo mantenedor da
documentação depois que o mantenedor da tradução fica satisfeito. Um
``Reviewed-by:`` do mantenedor da tradução costuma ser o sinal de que o
patch está pronto.

Os revisores normalmente verificam:

- se o patch se aplica e compila sem avisos novos;

- o cabeçalho do arquivo, os rótulos, as citações e as entradas de índice
  descritos acima;

- a fidelidade ao original;

- a gramática, o estilo e a consistência da terminologia;

- a mensagem de commit e as tags.

Todos são bem-vindos para revisar patches de tradução. Revisões de outros
falantes do idioma são valiosas, e revisar o trabalho de outras pessoas é
também uma das melhores formas de aprender as convenções de uma tradução.
Se você revisou um patch com cuidado e concorda com ele, diga isso com uma
tag ``Reviewed-by:``.

Tenha paciência. Os mantenedores das traduções são voluntários, e pode
levar uma ou duas semanas para receber uma resposta, mais durante a janela
de merge.


Mantendo as traduções atualizadas
=================================

A documentação em inglês muda o tempo todo, e uma tradução desatualizada
pode ser pior do que nenhuma. O ``tools/docs/checktransupdate.py`` lista,
para cada arquivo traduzido, os commits no original que ainda não estão
refletidos na tradução::

  $ tools/docs/checktransupdate.py -l <lang>
  $ tools/docs/checktransupdate.py Documentation/translations/<lang>/process/howto.rst

Para atualizar uma tradução, aplique as mesmas mudanças que os commits
listados fizeram no original, e nada além disso. Mencione esses commits na
mensagem de commit, terminando com a frase "Update the translation through
commit" descrita acima, para que a próxima execução do script comece do
ponto certo.

Quando um documento original for movido ou renomeado, mova a tradução da
mesma forma, para que os caminhos continuem coincidindo. Quando um
original for removido, remova também a tradução.


Iniciando uma nova tradução
===========================

Um novo idioma é um compromisso de longo prazo: alguém precisa revisar as
contribuições e manter os documentos existentes atualizados por anos.
Antes de iniciar um, discuta a ideia na lista de discussão linux-doc.

A primeira série de patches de uma nova tradução deve conter um pequeno
número de documentos traduzidos com cuidado, que compilem sem avisos, além
da infraestrutura de que toda tradução precisa:

- um diretório ``Documentation/translations/<ll_CC>/``, com o nome formado
  pelos códigos do idioma e do país (por exemplo, ``pt_BR``);

- um ``index.rst`` para o idioma, com uma tradução do aviso de
  :ref:`Documentation/translations/index.rst <translations_disclaimer>`,
  normalmente em um arquivo separado incluído pelo índice;

- uma entrada no ``toctree`` de Documentation/translations/index.rst;

- uma entrada no dicionário ``all_languages`` de
  ``Documentation/sphinx/translations.py``, que gera os links de idioma no
  topo de cada página.

Essas duas últimas listas são ordenadas alfabeticamente pelo nome do
idioma em inglês; mantenha-as assim.

Use o índice de uma tradução existente como modelo. Note que as diretivas
``raw:: latex`` encontradas nesses arquivos selecionam as fontes usadas na
saída em PDF, e que os idiomas escritos com caracteres do leste asiático
precisam de configurações diferentes das dos outros.

Uma entrada no MAINTAINERS para a nova tradução não faz parte do primeiro
envio. Ela é algo que se conquista com um esforço demonstrado e
contínuo, e é melhor discuti-la com o mantenedor da documentação depois
que a tradução já existir há algum tempo.


Lista de verificação
====================

Antes de enviar um patch de tradução, verifique se:

- o documento original ainda não foi traduzido e ninguém mais está
  trabalhando nele;

- o arquivo reproduz o caminho e o nome do original;

- o cabeçalho segue as convenções do seu idioma, começando pelo
  identificador SPDX;

- os rótulos do início do arquivo original foram removidos, e todos os
  rótulos e nomes de citações restantes têm o prefixo do idioma;

- as referências apontam para os documentos traduzidos, quando existirem,
  e para os documentos em inglês, caso contrário;

- os blocos literais, o código, os identificadores e os destinos das
  referências estão inalterados;

- a estrutura do original foi preservada e as linhas cabem em 80 colunas;

- a tradução é fiel, gramaticalmente correta e usa a terminologia
  estabelecida;

- o documento está listado no ``index.rst`` correto;

- a compilação completa com ``make htmldocs`` não acrescenta nenhum aviso,
  e a página HTML aparece corretamente;

- o ``scripts/checkpatch.pl --strict`` não aponta nenhum problema;

- a mensagem de commit está em inglês, descreve a mudança e diz em qual
  commit do original a tradução se baseia;

- todas as tags estão em um único bloco e o ``Signed-off-by:`` usa o seu
  nome real;

- o patch é enviado, em texto puro, para todos os listados pelo
  ``scripts/get_maintainer.pl``.
