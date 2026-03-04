.. SPDX-License-Identifier: GPL-2.0

.. include:: ../disclaimer-sp.rst

:Original: Documentation/rust/quick-start.rst
:Translator: Edwin Toribio <edwin.toribio.j@gmail.com>

.. _sp_rust_quick_start:

Guía de inicio rápido
=====================

Este documento describe cómo empezar con el desarrollo del kernel en Rust.

Existen varias formas de instalar el conjunto de herramientas (toolchain) de Rust
necesario para el desarrollo del kernel. Una forma sencilla es utilizar los
paquetes de su distribución de Linux si son adecuados; la primera sección a
continuación explica este enfoque. Una ventaja de este método es que,
normalmente, la distribución hará coincidir el LLVM utilizado por Rust y Clang.

Otra forma es utilizar las versiones estables precompiladas de LLVM+Rust
proporcionadas en `kernel.org <https://kernel.org/pub/tools/llvm/rust/>`_.
Estas son las mismas herramientas de LLVM ligeras y rápidas de
:ref:`Obtener LLVM <getting_llvm>` con versiones de Rust añadidas que son
compatibles con Rust para Linux. Se proporcionan dos conjuntos: el "LLVM más
reciente" (latest LLVM) y el "LLVM coincidente" (matching LLVM) (consulte el
enlace para más información).

Alternativamente, las dos secciones siguientes de "Requisitos" explican cada
componente y cómo instalarlos a través de ``rustup``, los instaladores
independientes de Rust y/o compilándolos.

El resto del documento explica otros aspectos sobre cómo empezar.


Distribuciones
--------------

Arch Linux
**********

Arch Linux proporciona versiones recientes de Rust y, por lo tanto, debería
funcionar directamente, p. ej.::

    pacman -S rust rust-src rust-bindgen


Debian
******

Debian 13 (Trixie), así como Testing y Debian Unstable (Sid) proporcionan
versiones recientes de Rust y, por lo tanto, deberían funcionar directamente, p. ej.::

    apt install rustc rust-src bindgen rustfmt rust-clippy


Fedora Linux
************

Fedora Linux proporciona versiones recientes de Rust y, por lo tanto, debería
funcionar directamente, p. ej.::

    dnf install rust rust-src bindgen-cli rustfmt clippy


Gentoo Linux
************

Gentoo Linux (y especialmente la rama de pruebas) proporciona versiones
recientes de Rust y, por lo tanto, debería funcionar directamente, p. ej.::

    USE='rust-src rustfmt clippy' emerge dev-lang/rust dev-util/bindgen

Es posible que sea necesario configurar ``LIBCLANG_PATH``.


Nix
***

Nix (canal unstable) proporciona versiones recientes de Rust y, por lo tanto,
debería funcionar directamente, p. ej.::

    { pkgs ? import <nixpkgs> {} }:
    pkgs.mkShell {
      nativeBuildInputs = with pkgs; [ rustc rust-bindgen rustfmt clippy ];
      RUST_LIB_SRC = "${pkgs.rust.packages.stable.rustPlatform.rustLibSrc}";
    }


openSUSE
********

openSUSE Slowroll y openSUSE Tumbleweed proporcionan versiones recientes de Rust
y, por lo tanto, deberían funcionar directamente, p. ej.::

    zypper install rust rust1.79-src rust-bindgen clang


Ubuntu
******

25.04
~~~~~

Las versiones más recientes de Ubuntu proporcionan versiones recientes de Rust
y, por lo tanto, deberían funcionar directamente, p. ej.::

    apt install rustc rust-src bindgen rustfmt rust-clippy

Además, es necesario configurar ``RUST_LIB_SRC``, p. ej.::

    RUST_LIB_SRC=/usr/src/rustc-$(rustc --version | cut -d' ' -f2)/library

Para mayor comodidad, ``RUST_LIB_SRC`` puede exportarse al entorno global.


24.04 LTS y anteriores
~~~~~~~~~~~~~~~~~~~~~~

Aunque Ubuntu 24.04 LTS y versiones anteriores todavía proporcionan versiones
recientes de Rust, requieren que se establezca alguna configuración adicional,
utilizando los paquetes con versión, p. ej.::

    apt install rustc-1.80 rust-1.80-src bindgen-0.65 rustfmt-1.80 \
        rust-1.80-clippy
    ln -s /usr/lib/rust-1.80/bin/rustfmt /usr/bin/rustfmt-1.80
    ln -s /usr/lib/rust-1.80/bin/clippy-driver /usr/bin/clippy-driver-1.80

Ninguno de estos paquetes establece sus herramientas como predeterminadas; por
lo tanto, deben especificarse explícitamente, p. ej.::

    make LLVM=1 RUSTC=rustc-1.80 RUSTDOC=rustdoc-1.80 RUSTFMT=rustfmt-1.80 \
        CLIPPY_DRIVER=clippy-driver-1.80 BINDGEN=bindgen-0.65

Alternativamente, modifique la variable ``PATH`` para colocar los binarios de
Rust 1.80 primero y establezca ``bindgen`` como predeterminado, p. ej.::

    PATH=/usr/lib/rust-1.80/bin:$PATH
    update-alternatives --install /usr/bin/bindgen bindgen \
        /usr/bin/bindgen-0.65 100
    update-alternatives --set bindgen /usr/bin/bindgen-0.65

``RUST_LIB_SRC`` debe configurarse cuando se usan los paquetes con versión, p. ej.::

    RUST_LIB_SRC=/usr/src/rustc-$(rustc-1.80 --version | cut -d' ' -f2)/library

Para mayor comodidad, ``RUST_LIB_SRC`` puede exportarse al entorno global.

Además, ``bindgen-0.65`` está disponible en versiones más nuevas (24.04 LTS y
24.10), pero puede no estar disponible en las más antiguas (20.04 LTS y 22.04 LTS),
por lo que es posible que sea necesario compilar ``bindgen`` manualmente
(consulte más abajo).


Requisitos: Compilación
-----------------------

Esta sección explica cómo obtener las herramientas necesarias para la compilación.

Para comprobar fácilmente si se cumplen los requisitos, se puede utilizar el
siguiente objetivo (target)::

    make LLVM=1 rustavailable

Esto activa la misma lógica utilizada por Kconfig para determinar si debe
habilitarse ``RUST_IS_AVAILABLE``; pero también explica por qué no en caso
de que así sea.


rustc
*****

Se requiere una versión reciente del compilador de Rust.

Si se está utilizando ``rustup``, entre en el directorio de compilación del
kernel (o use el argumento ``--path=<build-dir>`` para el subcomando ``set``)
y ejecute, por ejemplo::

    rustup override set stable

Esto configurará su directorio de trabajo para usar la versión dada de
``rustc`` sin afectar a su conjunto de herramientas predeterminado.

Tenga en cuenta que la anulación (override) se aplica al directorio de trabajo
actual (y sus subdirectorios).

Si no está utilizando ``rustup``, obtenga un instalador independiente de:

    https://forge.rust-lang.org/infra/other-installation-methods.html#standalone


Código fuente de la biblioteca estándar de Rust
***********************************************

Se requiere el código fuente de la biblioteca estándar de Rust porque el sistema
de compilación compilará de forma cruzada ``core``.

Si se está utilizando ``rustup``, ejecute::

    rustup component add rust-src

Los componentes se instalan por conjunto de herramientas, por lo que actualizar
la versión del compilador de Rust más adelante requiere volver a añadir el
componente.

De lo contrario, si se utiliza un instalador independiente, el árbol de fuentes
de Rust puede descargarse en la carpeta de instalación del conjunto de
herramientas::

    curl -L "https://static.rust-lang.org/dist/rust-src-$(rustc --version | cut -d' ' -f2).tar.gz" |
        tar -xzf - -C "$(rustc --print sysroot)/lib" \
        "rust-src-$(rustc --version | cut -d' ' -f2)/rust-src/lib/" \
        --strip-components=3

En este caso, actualizar la versión del compilador de Rust más adelante requiere
actualizar manualmente el árbol de fuentes (esto se puede hacer eliminando
``$(rustc --print sysroot)/lib/rustlib/src/rust`` y volviendo a ejecutar el
comando anterior).


libclang
********

``libclang`` (parte de LLVM) es utilizado por ``bindgen`` para entender el
código C en el kernel, lo que significa que LLVM debe estar instalado; al igual
que cuando el kernel se compila con ``LLVM=1``.

Es probable que las distribuciones de Linux tengan una adecuada disponible, por
lo que es mejor comprobarlo primero.

También hay algunos binarios para varios sistemas y arquitecturas subidos a:

    https://releases.llvm.org/download.html

De lo contrario, compilar LLVM lleva bastante tiempo, pero no es un proceso
complejo:

    https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm

Consulte Documentation/kbuild/llvm.rst para obtener más información y otras
formas de obtener versiones precompiladas y paquetes de distribuciones.


bindgen
*******

Los vínculos (bindings) con la parte de C del kernel se generan en tiempo de
compilación utilizando la herramienta ``bindgen``.

Instálela, por ejemplo, mediante (tenga en cuenta que esto descargará y
compilará la herramienta desde el código fuente)::

    cargo install --locked bindgen-cli

``bindgen`` utiliza la caja (crate) ``clang-sys`` para encontrar un
``libclang`` adecuado (que puede estar vinculado estáticamente, dinámicamente
o cargado en tiempo de ejecución). Por defecto, el comando ``cargo`` anterior
producirá un binario ``bindgen`` que cargará ``libclang`` en tiempo de
ejecución. Si no se encuentra (o si se debe usar un ``libclang`` diferente al
encontrado), el proceso se puede ajustar, p. ej. utilizando la variable de entorno
``LIBCLANG_PATH``. Para más detalles, consulte la documentación de
``clang-sys`` en:

    https://github.com/KyleMayes/clang-sys#linking

    https://github.com/KyleMayes/clang-sys#environment-variables


Requisitos: Desarrollo
----------------------

Esta sección explica cómo obtener las herramientas necesarias para el
desarrollo. Es decir, no son necesarias cuando solo se compila el kernel.


rustfmt
*******

La herramienta ``rustfmt`` se utiliza para formatear automáticamente todo el
código Rust del kernel, incluyendo los vínculos de C generados (para más
detalles, consulte coding-guidelines.rst).

Si se utiliza ``rustup``, su perfil ``default`` ya instala la herramienta, por
lo que no es necesario hacer nada. Si se utiliza otro perfil, el componente
puede instalarse manualmente::

    rustup component add rustfmt

Los instaladores independientes también vienen con ``rustfmt``.


clippy
******

``clippy`` es un linter de Rust. Ejecutarlo proporciona advertencias
adicionales para el código Rust. Se puede ejecutar pasando ``CLIPPY=1`` a
``make`` (para más detalles, consulte general-information.rst).

Si se utiliza ``rustup``, su perfil ``default`` ya instala la herramienta, por
lo que no es necesario hacer nada. Si se utiliza otro perfil, el componente
puede instalarse manualmente::

    rustup component add clippy

Los instaladores independientes también vienen con ``clippy``.


rustdoc
*******

``rustdoc`` es la herramienta de documentación para Rust. Genera una atractiva
documentación en HTML para el código Rust (para más detalles, consulte
general-information.rst).

``rustdoc`` también se utiliza para probar los ejemplos proporcionados en el
código Rust documentado (llamados doctests o pruebas de documentación). El
objetivo de Make ``rusttest`` utiliza esta función.

Si se utiliza ``rustup``, todos los perfiles ya instalan la herramienta, por
lo que no es necesario hacer nada.

Los instaladores independientes también vienen con ``rustdoc``.


rust-analyzer
*************

El servidor de lenguaje `rust-analyzer <https://rust-analyzer.github.io/>`_
puede utilizarse con muchos editores para habilitar el resaltado de sintaxis,
el completado, el salto a la definición y otras funciones.

``rust-analyzer`` necesita un archivo de configuración, ``rust-project.json``,
que puede generarse mediante el objetivo de Make ``rust-analyzer``::

    make LLVM=1 rust-analyzer


Configuración
-------------

El ``Rust support`` (``CONFIG_RUST``) debe habilitarse en el menú
``General setup``. La opción solo se muestra si se encuentra un conjunto de
herramientas de Rust adecuado (véase arriba), siempre que se cumplan los demás
requisitos. A su vez, esto hará visibles el resto de las opciones que
dependen de Rust.

A continuación, vaya a::

    Kernel hacking
        -> Sample kernel code
            -> Rust samples

Y habilite algunos módulos de ejemplo, ya sea como integrados (built-in) o
como cargables.


Compilación
-----------

Compilar un kernel con un conjunto de herramientas LLVM completo es la
configuración mejor soportada en este momento. Es decir::

    make LLVM=1

El uso de GCC también funciona para algunas configuraciones, pero es muy
experimental en este momento.


Hacking
-------

Para profundizar más, eche un vistazo al código fuente de los ejemplos en
``samples/rust/``, al código de soporte de Rust bajo ``rust/`` y al menú
``Rust hacking`` bajo ``Kernel hacking``.

Si se utiliza GDB/Binutils y los símbolos de Rust no se decodifican (demangle)
correctamente, la razón es que el conjunto de herramientas aún no soporta el
nuevo esquema de decoración de nombres (mangling) v0 de Rust. Hay algunas
soluciones:

- Instalar una versión más reciente (GDB >= 10.2, Binutils >= 2.36).

- Algunas versiones de GDB (p. ej. GDB 10.1 estándar) pueden utilizar los nombres
  pre-decodificados incrustados en la información de depuración(``CONFIG_DEBUG_INFO``).