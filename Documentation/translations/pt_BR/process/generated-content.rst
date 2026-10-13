.. SPDX-License-Identifier: GPL-2.0

=========================================================
Diretrizes do Kernel para Conteúdo Gerado por Ferramentas
=========================================================

Propósito
=========

Os contribuidores do kernel vêm usando ferramentas para gerar contribuições há
muito tempo. Essas ferramentas podem aumentar o volume de contribuições. Ao mesmo
tempo, o tempo disponível de revisores e mantenedores é um recurso escasso.
Entender quais partes de uma contribuição vêm de humanos e quais vêm de ferramentas
ajuda a preservar esses recursos e manter o desenvolvimento do kernel saudável.

O objetivo aqui é esclarecer as expectativas da comunidade em relação a ferramentas.
Isso permite que todos se tornem mais produtivos, mantendo ao mesmo tempo um alto
grau de confiança entre submetedores e revisores.

Fora do Escopo
===============

Estas diretrizes não se aplicam a ferramentas que fazem ajustes triviais em
conteúdo preexistente. Também não dizem respeito a ferramentas que ajudam em
tarefas menores. Alguns exemplos:

 - Correções de ortografia e gramática, como reformular para o modo imperativo
 - Auxílios de digitação como completar identificadores, boilerplate
   ou completar padrões triviais
 - Transformações puramente mecânicas, como renomear variáveis
 - Reformatação, como executar o Lindent, ``clang-format`` ou ``rust-fmt``

Mesmo quando o uso da sua ferramenta estiver fora do escopo, você
ainda deve sempre considerar se informar ao revisor qual ferramenta
você usou ajudaria na revisão da sua contribuição.

No Escopo
==========

Estas diretrizes se aplicam quando uma quantidade significativa de conteúdo em
uma contribuição do kernel não foi escrita por uma pessoa presente na cadeia de
Signed-off-by, mas sim criada por uma ferramenta.

A detecção de um problema e o teste da correção para ele também fazem parte do
processo de desenvolvimento; se uma ferramenta foi usada para encontrar um problema
tratado por uma alteração, isso deve ser mencionado no changelog. Isso não apenas
dá o devido crédito, como também ajuda outros desenvolvedores a descobrir essas
ferramentas.

Alguns exemplos:
 - Qualquer correção sugerida por ferramenta, como ``checkpatch.pl --fix``
 - Scripts do Coccinelle
 - Um chatbot gerou uma nova função no seu patch para ordenar entradas de lista.
 - Um arquivo .c do patch foi originalmente gerado por um assistente de codificação,
   mas foi limpo manualmente.
 - O changelog foi gerado entregando o patch a uma ferramenta de IA generativa e
   pedindo que ela escrevesse o changelog.
 - O changelog foi traduzido de outro idioma.

Em caso de dúvida, escolha a transparência e presuma que estas diretrizes se
aplicam à sua contribuição.

Diretrizes
==========

Primeiro, leia o Developer's Certificate of Origin:
Documentation/process/submitting-patches.rst. Suas regras são simples e estão em
vigor há muito tempo. Elas já cobriram diversas contribuições geradas por ferramentas.
Certifique-se de que você entende toda a sua submissão e está preparado para
responder aos comentários de revisão.

Segundo, ao fazer uma contribuição, seja transparente sobre a origem
do conteúdo em cover letters e changelogs. Você pode ser mais
transparente adicionando informações como:

 - Quais ferramentas foram usadas?
 - A entrada fornecida às ferramentas que você usou, como o script de origem do
   Coccinelle.
 - Se o código foi majoritariamente gerado a partir de um único prompt ou um
   pequeno conjunto de prompts, inclua esses prompts. Para sessões mais longas,
   inclua um resumo dos prompts e da natureza da assistência resultante.
 - Quais partes do conteúdo foram afetadas por essa ferramenta?
 - Como a submissão foi testada e quais ferramentas foram usadas para testar a
   correção?

Como em todas as contribuições, cada mantenedor tem a discricionariedade de escolher
como lidar com a contribuição. Por exemplo, eles podem:

 - Tratá-la como qualquer outra contribuição.
 - Rejeitá-la de imediato.
 - Tratar a contribuição de forma especial, por exemplo, pedindo testes extras,
   revisando com escrutínio adicional, ou revisando com prioridade menor do que
   conteúdo gerado por humanos.
 - Pedir algum outro passo especial, como solicitar que o contribuidor detalhe
   como a ferramenta ou o modelo foi treinado.
 - Pedir ao submetedor que explique em mais detalhes sobre a contribuição, para
   que o mantenedor possa se assegurar de que o submetedor entende completamente
   como o código funciona.
 - Sugerir um prompt melhor em vez de sugerir alterações específicas de código.

Se as ferramentas permitirem que você gere uma contribuição automaticamente,
espere escrutínio adicional proporcional a quanto dela foi gerado.

Como com a saída de qualquer ferramenta, o resultado pode estar incorreto ou
inadequado. Espera-se que você entenda e seja capaz de defender tudo o que submeter.
Se você não for capaz de fazer isso, então não submeta as alterações resultantes.

Se você o fizer mesmo assim, os mantenedores têm o direito de rejeitar sua série
sem revisão detalhada.

Referências
============

Para diretrizes específicas sobre assistentes de codificação de IA, veja:


* Documentation/process/coding-assistants.rst
