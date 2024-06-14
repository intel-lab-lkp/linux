
#define __app__(x, y) str__##x##y
#define __app(x, y) __app__(x, y)

#define TRACE_SYSTEM_STRING __app(TRACE_SYSTEM_VAR,__trace_system_name)

#define TRACE_MAKE_SYSTEM_STR()				\
	static const char TRACE_SYSTEM_STRING[] =	\
		__stringify(TRACE_SYSTEM)

TRACE_MAKE_SYSTEM_STR();

#undef TRACE_DEFINE_ENUM
#define TRACE_DEFINE_ENUM(a)				\
	static struct trace_eval_map __used __initdata	\
	__##TRACE_SYSTEM##_##a =			\
	{						\
		.system = TRACE_SYSTEM_STRING,		\
		.eval_string = #a,			\
		.eval_value = a				\
	};						\
	static struct trace_eval_map __used		\
	__section("_ftrace_eval_map")			\
	*TRACE_SYSTEM##_##a = &__##TRACE_SYSTEM##_##a

/*
 * Define a symbol for __print_sym by giving lookup and
 * show functions. See &struct trace_sym_def.
 */
#undef TRACE_DEFINE_SYM_FNS
#define TRACE_DEFINE_SYM_FNS(_symbol_id, _lookup, _show)		\
	_TRACE_DEFINE_SYM_FNS(TRACE_SYSTEM, _symbol_id, _lookup, _show)
#define _TRACE_DEFINE_SYM_FNS(_system, _symbol_id, _lookup, _show)	\
	__TRACE_DEFINE_SYM_FNS(_system, _symbol_id, _lookup, _show)
#define __TRACE_DEFINE_SYM_FNS(_system, _symbol_id, _lookup, _show)	\
	___TRACE_DEFINE_SYM_FNS(_system ## _ ## _symbol_id, _symbol_id,	\
				_lookup, _show)
#define ___TRACE_DEFINE_SYM_FNS(_name, _symbol_id, _lookup, _show)	\
	static struct trace_sym_def					\
	__trace_sym_def_ ## _name = {					\
		.system = TRACE_SYSTEM_STRING,				\
		.symbol_id = #_symbol_id,				\
		.lookup = _lookup,					\
		.show = _show,						\
	};								\
	static struct trace_sym_def 					\
	__section("_ftrace_sym_defs")					\
	*__trace_sym_def_p_ ## _name = &__trace_sym_def_ ## _name

/*
 * Define a symbol for __print_sym by giving lookup and
 * show functions. See &struct trace_sym_def.
 */
#undef TRACE_DEFINE_SYM_LIST
#define TRACE_DEFINE_SYM_LIST(_symbol_id, ...)				\
	_TRACE_DEFINE_SYM_LIST(TRACE_SYSTEM, _symbol_id, __VA_ARGS__)
#define _TRACE_DEFINE_SYM_LIST(_system, _symbol_id, ...)		\
	__TRACE_DEFINE_SYM_LIST(_system, _symbol_id, __VA_ARGS__)
#define __TRACE_DEFINE_SYM_LIST(_system, _symbol_id, ...)		\
	___TRACE_DEFINE_SYM_LIST(_system ## _ ## _symbol_id, _symbol_id,\
				 __VA_ARGS__)
#define ___TRACE_DEFINE_SYM_LIST(_name, _symbol_id, ...)		\
	static struct trace_sym_entry					\
	__trace_sym_list_ ## _name[] = { __VA_ARGS__ };			\
	static const char *						\
	__trace_sym_lookup_ ## _name(unsigned long long value)		\
	{								\
		return trace_sym_lookup(__trace_sym_list_ ## _name,	\
			ARRAY_SIZE(__trace_sym_list_ ## _name), value);	\
	}								\
	static void							\
	__trace_sym_show_ ## _name(struct seq_file *m)			\
	{								\
		trace_sym_show(m, __trace_sym_list_ ## _name,		\
			       ARRAY_SIZE(__trace_sym_list_ ## _name));	\
	}								\
	___TRACE_DEFINE_SYM_FNS(_name, _symbol_id,			\
				__trace_sym_lookup_ ## _name,		\
				__trace_sym_show_ ## _name)

#undef TRACE_DEFINE_SIZEOF
#define TRACE_DEFINE_SIZEOF(a)				\
	static struct trace_eval_map __used __initdata	\
	__##TRACE_SYSTEM##_##a =			\
	{						\
		.system = TRACE_SYSTEM_STRING,		\
		.eval_string = "sizeof(" #a ")",	\
		.eval_value = sizeof(a)			\
	};						\
	static struct trace_eval_map __used		\
	__section("_ftrace_eval_map")			\
	*TRACE_SYSTEM##_##a = &__##TRACE_SYSTEM##_##a
