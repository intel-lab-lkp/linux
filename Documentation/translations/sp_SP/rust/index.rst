.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-sp.rst

:Original: Documentation/rust/quick-start.rst
:Translator: Edwin Toribio <edwin.toribio.j@gmail.com>

.. _sp_rust_index:

Rust
====

Documentación relacionada con Rust dentro del kernel. Para empezar a usar Rust
en el kernel, por favor lea la guía quick-start.rst.

Documentación del código
------------------------

Dada una configuración del kernel, el kernel puede generar documentación del
código Rust, p. ej., HTML renderizado por la herramienta ``rustdoc``.

.. only:: rustdoc and html

    Esta documentación del kernel fue construida con la `documentación del código Rust
    <rustdoc/kernel/index.html>`_.

.. only:: not rustdoc and html

    Esta documentación del kernel no fue construida con la documentación del código Rust.

Se proporciona una versión pregenerada en:

    https://rust.docs.kernel.org

Por favor, consulte la sección :ref:`Documentación del código <rust_code_documentation>`
para más detalles.

.. toctree::
    :maxdepth: 1

    quick-start
    general-information
    coding-guidelines

También puede encontrar materiales de aprendizaje para Rust en su sección en
:doc:`../process/kernel-docs`.