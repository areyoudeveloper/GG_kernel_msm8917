/*
 *  drivers/cpufreq/sad_tea.h
 *  header For cpufreq_sad_tea.c
 *  Copyright (C) 2020 tea <kmd36rk@gmail.com>
 */
#ifndef _SAD_TEA_H
#define _SAD_TEA_H

#include <linux/cpufreq.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define MSRR			(1)
#define TLL		(5 * 500 * 500)

enum {TEA_NORMAL_SAMPLE, TEA_SUB_SAMPLE};

/* Create attributes */
#define gov_sys_attr_ro(_name)						\
static struct global_attr _name##_gov_sys =				\
__ATTR(_name, 0444, show_##_name##_gov_sys, NULL)

#define gov_sys_attr_rw(_name)						\
static struct global_attr _name##_gov_sys =				\
__ATTR(_name, 0644, show_##_name##_gov_sys, store_##_name##_gov_sys)

#define gov_pol_attr_ro(_name)						\
static struct freq_attr _name##_gov_pol =				\
__ATTR(_name, 0444, show_##_name##_gov_pol, NULL)

#define gov_pol_attr_rw(_name)						\
static struct freq_attr _name##_gov_pol =				\
__ATTR(_name, 0644, show_##_name##_gov_pol, store_##_name##_gov_pol)

#define gov_sys_pol_attr_rw(_name)					\
	gov_sys_attr_rw(_name);						\
	gov_pol_attr_rw(_name)

#define gov_sys_pol_attr_ro(_name)					\
	gov_sys_attr_ro(_name);						\
	gov_pol_attr_ro(_name)

/* Create show/store routines */
#define show_one(_gov, file_name)					\
static ssize_t show_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	struct _gov##_dbs_tuners *tuners = _gov##_dbs_cdata.gdbs_data->tuners; \
	return sprintf(buf, "%u\n", tuners->file_name);			\
}									\
									\
static ssize_t show_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, char *buf)				\
{									\
	struct dbs_data *dbs_data = policy->governor_data;		\
	struct _gov##_dbs_tuners *tuners = dbs_data->tuners;		\
	return sprintf(buf, "%u\n", tuners->file_name);			\
}

#define store_one(_gov, file_name)					\
static ssize_t store_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count) \
{									\
	struct dbs_data *dbs_data = _gov##_dbs_cdata.gdbs_data;		\
	return store_##file_name(dbs_data, buf, count);			\
}									\
									\
static ssize_t store_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	struct dbs_data *dbs_data = policy->governor_data;		\
	return store_##file_name(dbs_data, buf, count);			\
}

#define show_store_one(_gov, file_name)					\
show_one(_gov, file_name);						\
store_one(_gov, file_name)

/* create helper routines */
#define define_get_cpu_dbs_routines(_dbs_info)				\
static struct cpu_dbs_common_info *get_cpu_cdbs(int cpu)		\
{									\
	return &per_cpu(_dbs_info, cpu).cdbs;				\
}									\
									\
static void *get_cpu_dbs_info_s(int cpu)				\
{									\
	return &per_cpu(_dbs_info, cpu);				\
}


struct cpu_dbs_common_info {
	int cpu;
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
	u64 prev_cpu_nice;
	unsigned int prev_load;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	struct mutex timer_mutex;
	ktime_t time_stamp;
};

struct tea_cpu_dbs_info_s {
	struct cpu_dbs_common_info cdbs;
	struct cpufreq_frequency_table *freq_table;
	unsigned int freq_lo;
	unsigned int freq_lo_jiffies;
	unsigned int freq_hi_jiffies;
	unsigned int rate_mult;
	unsigned int sample_type:1;
};

struct tea_dbs_tuners {
	unsigned int ignore_nice_load;
	unsigned int sampling_rate;
	unsigned int sampling_down_factor;
	unsigned int up_threshold;
	unsigned int powersave_bias;
	unsigned int io_is_busy;
};

struct dbs_data;
struct common_dbs_data {
    #define GOV_SAD_TEA		0
	int governor;
	struct attribute_group *attr_group_gov_sys; 
	struct attribute_group *attr_group_gov_pol; 

struct dbs_data *gdbs_data;

	struct cpu_dbs_common_info *(*get_cpu_cdbs)(int cpu);
	void *(*get_cpu_dbs_info_s)(int cpu);
	void (*gov_dbs_timer)(struct work_struct *work);
	void (*gov_check_cpu)(int cpu, unsigned int load);
	int (*init)(struct dbs_data *dbs_data);
	void (*exit)(struct dbs_data *dbs_data);

	void *gov_ops;
};

struct dbs_data {
	struct common_dbs_data *cdata;
	unsigned int min_sampling_rate;
	int usage_count;
	void *tuners;

	struct mutex mutex;
};

struct tea_ops {
	void (*powersave_bias_init_cpu)(int cpu);
	unsigned int (*powersave_bias_target)(struct cpufreq_policy *policy,
			unsigned int freq_next, unsigned int relation);
	void (*freq_increase)(struct cpufreq_policy *policy, unsigned int freq);
};

static inline int delay_for_sampling_rate(unsigned int sampling_rate)
{
	int delay = usecs_to_jiffies(sampling_rate);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	return delay;
}

#define declare_show_sampling_rate_min(_gov)				\
static ssize_t show_sampling_rate_min_gov_sys				\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	struct dbs_data *dbs_data = _gov##_dbs_cdata.gdbs_data;		\
	return sprintf(buf, "%u\n", dbs_data->min_sampling_rate);	\
}									\
									\
static ssize_t show_sampling_rate_min_gov_pol				\
(struct cpufreq_policy *policy, char *buf)				\
{									\
	struct dbs_data *dbs_data = policy->governor_data;		\
	return sprintf(buf, "%u\n", dbs_data->min_sampling_rate);	\
}

extern struct mutex cpufreq_governor_lock;

void dbs_check_cpu(struct dbs_data *dbs_data, int cpu);
bool need_load_eval(struct cpu_dbs_common_info *cdbs,
		unsigned int sampling_rate);
int cpufreq_governor_dbs(struct cpufreq_policy *policy,
		struct common_dbs_data *cdata, unsigned int event);
void gov_queue_work(struct dbs_data *dbs_data, struct cpufreq_policy *policy,
		unsigned int delay, bool all_cpus);
void tea_register_powersave_bias_handler(unsigned int (*f)
		(struct cpufreq_policy *, unsigned int, unsigned int),
		unsigned int powersave_bias);
void tea_unregister_powersave_bias_handler(void);
#endif /* _SAD_TEA_H */
