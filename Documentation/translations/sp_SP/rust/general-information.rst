.. SPDX-License-Identifier: GPL-2.0

.. include:: ../disclaimer-sp.rst

:Original: Documentation/rust/general-information.rst
:Translator: Edwin Toribio <edwin.toribio.j@gmail.com>

.. _sp_rust_general_information:

Información general
===================

Este documento contiene información útil que conviene conocer cuando se trabaja
con el soporte de Rust en el kernel.


``no_std``
----------

El soporte de Rust en el kernel solo puede vincularse a `core <https://doc.rust-lang.org/core/>`_,
pero no a `std <https://doc.rust-lang.org/std/>`_. Las cajas (crates) destinadas
al uso en el kernel deben optar por este comportamiento utilizando el atributo
``#![no_std]``.


.. _sp_rust_code_documentation:

Documentación del código
------------------------

El código Rust del kernel se documenta mediante ``rustdoc``, su generador de
documentación integrado.

Los documentos HTML generados incluyen búsqueda integrada, elementos enlazados
(p. ej. tipos, funciones, constantes), código fuente, etc. Pueden leerse en:

    https://rust.docs.kernel.org

Para linux-next, consulte:

    https://rust.docs.kernel.org/next/

También existen etiquetas para cada lanzamiento principal, p. ej.:

    https://rust.docs.kernel.org/6.10/

La documentación también puede generarse y leerse fácilmente de forma local.
Esto es bastante rápido (del mismo orden que compilar el código en sí) y no se
necesitan herramientas ni entornos especiales. Esto tiene la ventaja añadida de
que estará adaptada a la configuración particular del kernel utilizada. Para
generarla, utilice el objetivo ``rustdoc`` con la misma invocación utilizada
para la compilación, p. ej.::

    make LLVM=1 rustdoc

Para leer la documentación localmente en su navegador web, ejecute por ejemplo::

    xdg-open Documentation/output/rust/rustdoc/kernel/index.html

Para aprender cómo escribir la documentación, consulte coding-guidelines.rst.


Lints adicionales
-----------------

Aunque ``rustc`` es un compilador muy útil, existen algunos lints y análisis
adicionales disponibles a través de ``clippy``, un linter de Rust. Para
habilitarlo, pase ``CLIPPY=1`` a la misma invocación utilizada para la
compilación, p. ej.::

    make LLVM=1 CLIPPY=1

Tenga en cuenta que Clippy puede cambiar la generación de código, por lo que no
debe habilitarse mientras se compila un kernel de producción.


Abstracciones vs. vínculos (bindings)
-------------------------------------

Las abstracciones son código Rust que envuelve la funcionalidad del kernel desde
el lado de C.

Para poder utilizar funciones y tipos del lado de C, se crean vínculos
(bindings). Los vínculos son las declaraciones para Rust de esas funciones y
tipos del lado de C.

Por ejemplo, se podría escribir una abstracción ``Mutex`` en Rust que envuelva
un ``struct mutex`` del lado de C y llame a sus funciones a través de los
vínculos.

Las abstracciones no están disponibles para todas las API internas y conceptos
del kernel, pero se pretende que la cobertura se amplíe con el tiempo. Los
módulos hoja ("Leaf") (p. ej. controladores/drivers) no deben utilizar los vínculos de C
directamente. En su lugar, los subsistemas deben proporcionar abstracciones tan
seguras como sea posible según sea necesario.

.. code-block::

                                                    rust/bindings/
                                                   (rust/helpers/)

                                                       include/ -----+ <-+
                                                                     |   |
      drivers/              rust/kernel/              +----------+ <-+   |
        fs/                                           | bindgen  |       |
       .../            +-------------------+          +----------+ --+   |
                       |    Abstracciones  |                         |   |
    +---------+        | +------+ +------+ |          +----------+   |   |
    | mi_foo  | -----> | | foo  | | bar  | | -------> | Vínculos | <-+   |
    | driver  | Segura | | sub- | | sub- | | Insegura |(Bindings)|       |
    +---------+        | |sist. | |sist. | |          |          |       |
         |             | +------+ +------+ |          |  caja    | <-----+
         |             |   caja del kernel |          | bindings |       |
         |             +-------------------+          +----------+       |
         |                                                               |
         +------------------# PROHIBIDO #--------------------------------+

La idea principal es encapsular toda interacción directa con las API de C del
kernel en abstracciones cuidadosamente revisadas y documentadas. Entonces, los
usuarios de estas abstracciones no podrán introducir un comportamiento
indefinido (UB) siempre que:

#. Las abstracciones sean correctas ("sound").
#. Cualquier bloque ``unsafe`` respete el contrato de seguridad necesario para
   llamar a las operaciones dentro del bloque. Del mismo modo, cualquier
   implementación ``unsafe impl`` respete el contrato de seguridad necesario
   para implementar el rasgo (trait).

Vínculos (Bindings)
~~~~~~~~~~~~~~~~~~~

Al incluir una cabecera C de ``include/`` en
``rust/bindings/bindings_helper.h``, la herramienta ``bindgen`` generará
automáticamente los vínculos para el subsistema incluido. Tras la compilación,
vea los archivos de salida ``*_generated.rs`` en el directorio
``rust/bindings/``.

Para las partes de la cabecera C que ``bindgen`` no genera automáticamente, p. ej.
funciones ``inline`` de C o macros no triviales, es aceptable añadir una pequeña
función de envoltura (wrapper) en ``rust/helpers/`` para que esté disponible
también para el lado de Rust.

Abstracciones
~~~~~~~~~~~~~

Las abstracciones son la capa entre los vínculos y los usuarios dentro del
kernel. Se encuentran en ``rust/kernel/`` y su función es encapsular el acceso
inseguro (unsafe) a los vínculos en una API lo más segura posible que exponen a
sus usuarios. Los usuarios de las abstracciones incluyen elementos como
controladores o sistemas de archivos escritos en Rust.

Además del aspecto de la seguridad, se supone que las abstracciones deben ser
"ergonómicas", en el sentido de que convierten las interfaces de C en código
Rust "idiomático". Ejemplos básicos son convertir la adquisición y liberación
de recursos de C en constructores y destructores de Rust, o los códigos de error
enteros de C en los tipos ``Result`` de Rust.


Compilación condicional
-----------------------

El código Rust tiene acceso a la compilación condicional basada en la
configuración del kernel:

.. code-block:: rust

    #[cfg(CONFIG_X)]       // Habilitado              (`y` o `m`)
    #[cfg(CONFIG_X="y")]   // Habilitado como integrado (`y`)
    #[cfg(CONFIG_X="m")]   // Habilitado como módulo    (`m`)
    #[cfg(not(CONFIG_X))]  // Deshabilitado

Para otros predicados que el ``cfg`` de Rust no soporta, p. ej. expresiones con
comparaciones numéricas, se puede definir un nuevo símbolo Kconfig:

.. code-block:: kconfig

    config RUSTC_VERSION_MIN_107900
        def_bool y if RUSTC_VERSION >= 107900