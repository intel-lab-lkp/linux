// SPDX-License-Identifier: GPL-2.0
#include <linux/static_call.h>
#include <linux/init.h>

long __static_call_return0(void)
{
	return 0;
}
EXPORT_SYMBOL_GPL(__static_call_return0);

#ifdef CONFIG_STATIC_CALL_SELFTEST

static int func_a(int x)
{
	return x+1;
}

static int func_b(int x)
{
	return x+2;
}

DEFINE_STATIC_CALL(sc_selftest, func_a);

static struct static_call_data {
      int (*func)(int);
      int val;
      int expect;
} static_call_data [] __initdata = {
      { NULL,   2, 3 },
      { func_b, 2, 4 },
      { func_a, 2, 3 }
};

static int __init test_static_call_init(void)
{
      int i;

      for (i = 0; i < ARRAY_SIZE(static_call_data); i++ ) {
	      struct static_call_data *scd = &static_call_data[i];

              if (scd->func)
                      static_call_update(sc_selftest, scd->func);

              WARN_ON(static_call(sc_selftest)(scd->val) != scd->expect);
      }

      return 0;
}
early_initcall(test_static_call_init);

#endif /* CONFIG_STATIC_CALL_SELFTEST */
