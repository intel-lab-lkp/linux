/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit API to allow symbols to be conditionally visible during KUnit
 * testing
 *
 * Copyright (C) 2022, Google LLC.
 * Author: Rae Moar <rmoar@google.com>
 */

#ifndef _KUNIT_VISIBILITY_H
#define _KUNIT_VISIBILITY_H

#if IS_ENABLED(CONFIG_KUNIT)
    /**
     * DECLARE_IF_KUNIT - Conditionally introduce identifiers
     * @body: identifiers to be introduced conditionally
     *
     * This macro introduces identifiers only if CONFIG_KUNIT is enabled.
     * Otherwise if CONFIG_KUNIT is not enabled no identifiers will be defined.
     *
     * .. code-block:: C
     *
     *     struct example {
     *         // @foo: regular data
     *         int foo;
     *
     *         // private: data available only for CONFIG_KUNIT
     *         DECLARE_IF_KUNIT(int bar);
     *     };
     */
    #define DECLARE_IF_KUNIT(body...)	body

    /**
     * VALUE_IF_KUNIT - Conditionally evaluate an expression
     * @expr: the expression to be evaluated conditionally
     *
     * This macro evaluates expression statement only if CONFIG_KUNIT is enabled.
     * Otherwise if CONFIG_KUNIT is not enabled it will evaluate always to 0.
     *
     * .. code-block:: C
     *
     *     int real_func(int i)
     *     {
     *         if (VALUE_IF_KUNIT(i == 0xC0FFE))
     *             return 0;
     *
     *         return i + 1;
     *     }
     */
    #define VALUE_IF_KUNIT(expr...)	expr

    /**
     * VISIBLE_IF_KUNIT - A macro that sets symbols to be static if
     * CONFIG_KUNIT is not enabled. Otherwise if CONFIG_KUNIT is enabled
     * there is no change to the symbol definition.
     */
    #define VISIBLE_IF_KUNIT
    /**
     * EXPORT_SYMBOL_IF_KUNIT(symbol) - Exports symbol into
     * EXPORTED_FOR_KUNIT_TESTING namespace only if CONFIG_KUNIT is
     * enabled. Must use MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING)
     * in test file in order to use symbols.
     */
    #define EXPORT_SYMBOL_IF_KUNIT(symbol) EXPORT_SYMBOL_NS(symbol, \
	    EXPORTED_FOR_KUNIT_TESTING)
#else
    #define DECLARE_IF_KUNIT(body...)
    #define VALUE_IF_KUNIT(expr...) 0
    #define VISIBLE_IF_KUNIT static
    #define EXPORT_SYMBOL_IF_KUNIT(symbol)
#endif

#endif /* _KUNIT_VISIBILITY_H */
