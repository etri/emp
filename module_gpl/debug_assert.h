#ifndef __DEBUG_ASSERT_H__
#define __DEBUG_ASSERT_H__
#ifdef CONFIG_EMP_DEBUG
	#define dprintk(...) printk(KERN_ERR __VA_ARGS__)
	#define dprintk_ratelimited(...) printk_ratelimited(KERN_ERR __VA_ARGS__)
	#define COMPILER_DEBUG __attribute__((optimize("-O0")))
	#define debug_assert(c) do { BUG_ON(!(c)); } while (0)
	#define debug_BUG() do { BUG(); } while (0)
	#define debug_BUG_ON(c) do { BUG_ON(c); } while (0)
	#define debug_WARN(...) WARN(__VA_ARGS__)
	#define debug_WARN_ONCE(...) WARN_ONCE(__VA_ARGS__)
#else
	#define dprintk(...) do {} while (0)
	#define dprintk_ratelimited(...) do {} while (0)
	#define COMPILER_DEBUG
	#define debug_assert(c) do {} while (0)
	#define debug_BUG() do {} while (0)
	#define debug_BUG_ON(c) do {} while (0)
	#define debug_WARN(cond, fmt...) do {} while (0)
	#define debug_WARN_ONCE(cond, fmt...) (unlikely(cond))
#endif
#endif /* __DEBUG_ASSERT_H__ */
