#ifdef CONFIG_EMP_STAT
#ifndef __STAT_H__
#define __STAT_H__
static inline void inc_vma_fault(struct vcpu_var *v)
{
	v->stat.vma_fault++;
}

static inline void inc_local_fault(struct vcpu_var *v)
{
	v->stat.local_fault++;
}

static inline void inc_remote_fault(struct vcpu_var *v)
{
	v->stat.remote_fault++;
}
#endif /* __STAT_H__ */
#endif
