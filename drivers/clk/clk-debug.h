/* SPDX-License-Identifier: GPL-2.0 */

struct clk;
struct clk_duty;
struct clk_hw;
struct seq_file;

extern int clk_hw_enable_state(struct clk_hw *hw);
extern unsigned int clk_hw_enable_count(struct clk_hw *hw);
extern unsigned int clk_hw_prepare_count(struct clk_hw *hw);
extern unsigned int clk_hw_protect_count(struct clk_hw *hw);
extern int clk_hw_get_phase(struct clk_hw *hw);
extern int clk_hw_get_scaled_duty_cycle(struct clk_hw *hw, unsigned int scale);
extern void clk_hw_get_duty(struct clk_hw *hw, struct clk_duty *duty);
extern unsigned long clk_hw_get_rate_recalc(struct clk_hw *hw);
extern long clk_hw_get_accuracy_recalc(struct clk_hw *hw);
extern void clk_debug_get_rate_range(struct clk_hw *hw, unsigned long *min_rate,
				     unsigned long *max_rate);
extern unsigned int clk_hw_notifier_count(struct clk_hw *hw);

extern const char *clk_con_id(struct clk *clk);
extern const char *clk_dev_id(struct clk *clk);
extern struct clk *clk_hw_next_consumer(struct clk_hw *hw, struct clk *prev);
extern void clk_hw_show_parent_by_index(struct seq_file *s, struct clk_hw *hw,
					unsigned int i, char terminator);

extern int clk_show_tree(void (*show_fn)(struct clk_hw *hw, int level,
					 int next_level, bool first,
					 void *data),
			 void *data, bool orphan_only);

extern void clk_hw_debug_for_each_init(struct dentry *(*fn)(struct clk_hw *hw));
extern void clk_hw_debug_exit(void);
