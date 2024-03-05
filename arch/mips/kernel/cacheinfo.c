// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPS cacheinfo support
 */
#include <linux/cacheinfo.h>

/* Populates leaf and increments to next leaf */
#define populate_cache(cache, leaf, c_level, c_type)		\
do {								\
	leaf->type = c_type;					\
	leaf->level = c_level;					\
	leaf->coherency_line_size = c->cache.linesz;		\
	leaf->number_of_sets = c->cache.sets;			\
	leaf->ways_of_associativity = c->cache.ways;		\
	leaf->size = c->cache.linesz * c->cache.sets *		\
		c->cache.ways;					\
	leaf++;							\
} while (0)

static int __init_cache_level(unsigned int cpu)
{
	struct cpuinfo_mips *c = &current_cpu_data;
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	int levels = 0, leaves = 0;

	/*
	 * If Dcache is not set, we assume the cache structures
	 * are not properly initialized.
	 */
	if (c->dcache.waysize)
		levels += 1;
	else
		return -ENOENT;


	leaves += (c->icache.waysize) ? 2 : 1;

	if (c->scache.waysize) {
		levels++;
		leaves++;
	}

	if (c->tcache.waysize) {
		levels++;
		leaves++;
	}

	this_cpu_ci->num_levels = levels;
	this_cpu_ci->num_leaves = leaves;
	return 0;
}

static void fill_cpumask_siblings(int cpu, cpumask_t *cpu_map)
{
	int cpu1;

	for_each_possible_cpu(cpu1)
		if (cpus_are_siblings(cpu, cpu1))
			cpumask_set_cpu(cpu1, cpu_map);
}

static void fill_cpumask_cluster(int cpu, cpumask_t *cpu_map)
{
	int cpu1;
	int cluster = cpu_cluster(&cpu_data[cpu]);

	for_each_possible_cpu(cpu1)
		if (cpu_cluster(&cpu_data[cpu1]) == cluster)
			cpumask_set_cpu(cpu1, cpu_map);
}

static int __populate_cache_leaves(unsigned int cpu)
{
	struct cpuinfo_mips *c = &current_cpu_data;
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	struct cacheinfo *this_leaf = this_cpu_ci->info_list;

	if (c->icache.waysize) {
		/* L1 caches are per core */
		fill_cpumask_siblings(cpu, &this_leaf->shared_cpu_map);
		populate_cache(dcache, this_leaf, 1, CACHE_TYPE_DATA);
		fill_cpumask_siblings(cpu, &this_leaf->shared_cpu_map);
		populate_cache(icache, this_leaf, 1, CACHE_TYPE_INST);
	} else {
		populate_cache(dcache, this_leaf, 1, CACHE_TYPE_UNIFIED);
	}

	if (c->scache.waysize) {
		/* L2 cache is per cluster */
		fill_cpumask_cluster(cpu, &this_leaf->shared_cpu_map);
		populate_cache(scache, this_leaf, 2, CACHE_TYPE_UNIFIED);
	}

	if (c->tcache.waysize)
		populate_cache(tcache, this_leaf, 3, CACHE_TYPE_UNIFIED);

	this_cpu_ci->cpu_map_populated = true;

	return 0;
}

DEFINE_SMP_CALL_CACHE_FUNCTION(init_cache_level)
DEFINE_SMP_CALL_CACHE_FUNCTION(populate_cache_leaves)
