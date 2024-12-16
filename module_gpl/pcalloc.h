#ifndef __PCALLOC_H__
#define __PCALLOC_H__
#include <linux/cpumask.h>
#include <linux/topology.h>

static inline void __pcfree_data(void **arr)
{
	int cpu;

	if (unlikely(arr == NULL))
		return;

	for_each_possible_cpu(cpu) {
		if (likely(arr[cpu]))
			emp_kfree_node(arr[cpu]);
	}
	emp_kfree(arr);
}

static inline void **__pcalloc_data(int size)
{
	int cpu;
	void **arr;

	arr = (void **)emp_kmalloc(sizeof(void *) * num_possible_cpus(),
				   GFP_KERNEL);
	if (unlikely(arr == NULL))
		return NULL;

	memset(arr, 0, sizeof(void *) * num_possible_cpus());
	for_each_possible_cpu(cpu) {
		arr[cpu] = (void *)emp_kmalloc_node(size, GFP_KERNEL,
						    cpu_to_node(cpu));
		if (unlikely(arr[cpu] == NULL))
			goto error;

		memset(arr[cpu], 0, size);
	}
	return arr;

error:
	__pcfree_data(arr);
	return NULL;
}

#define emp_free_pcdata(array) \
	__pcfree_data((void **)array)
#define emp_alloc_pcdata(type) \
	(typeof(type) **)__pcalloc_data(sizeof(type) + __alignof__(type))

#define emp_ptr_pcdata(array, cpu) ((typeof(*(array)))array[cpu])
#define emp_pc_ptr(array, cpu) ((typeof(*(array)))array[cpu])

#define emp_this_cpu_ptr(var) ({ \
	typeof(var) ____ret; \
	preempt_disable(); \
	____ret = this_cpu_ptr(var); \
	preempt_enable(); \
	____ret; \
})

#endif
