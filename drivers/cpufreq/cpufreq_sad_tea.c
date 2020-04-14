/*
 *  drivers/cpufreq/cpufreq_sad_tea.c
 *  Copyright (C) 2020 tea <kmd36rk@gmail.com>
 *  This cpu freq based on ondemand gov
 *  we do little optimization here
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include "sad_tea.h"

/* sad_tea governor macros */
#define DEF_FREQUENCY_UP_THRESHOLD		(20)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(100000)
#define MICRO_FREQUENCY_UP_THRESHOLD		(85)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)
#define MIN_FREQUENCY_UP_THRESHOLD		(10)
#define MAX_FREQUENCY_UP_THRESHOLD		(80)

static DEFINE_PER_CPU(struct tea_cpu_dbs_info_s, tea_cpu_dbs_info);

static struct tea_ops tea_ops;

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SAD_TEA
static struct cpufreq_governor cpufreq_gov_sad_tea;
#endif

static unsigned int default_powersave_bias;

static void sad_tea_powersave_bias_init_cpu(int cpu)
{
	struct tea_cpu_dbs_info_s *dbs_info = &per_cpu(tea_cpu_dbs_info, cpu);

	dbs_info->freq_table = cpufreq_frequency_get_table(cpu);
	dbs_info->freq_lo = 0;
}
/*
 * Find right freq to be set now with powersave_bias on.
 * Returns the freq_hi to be used right now and will set freq_hi_jiffies,
 * freq_lo, and freq_lo_jiffies in percpu area for averaging freqs.
 */
static unsigned int generic_powersave_bias_target(struct cpufreq_policy *policy,
		unsigned int freq_next, unsigned int relation)
{
	unsigned int freq_req, freq_reduc, freq_avg;
	unsigned int freq_hi, freq_lo;
	unsigned int index = 0;
	unsigned int jiffies_total, jiffies_hi, jiffies_lo;
	struct tea_cpu_dbs_info_s *dbs_info = &per_cpu(tea_cpu_dbs_info,
						   policy->cpu);
	struct dbs_data *dbs_data = policy->governor_data;
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;

	if (!dbs_info->freq_table) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_next;
	}

	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_next,
			relation, &index);
	freq_req = dbs_info->freq_table[index].frequency;
	freq_reduc = freq_req * tea_tuners->powersave_bias / 500;
	freq_avg = freq_req - freq_reduc;

	/* Find freq bounds for freq_avg in freq_table */
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_H, &index);
	freq_lo = dbs_info->freq_table[index].frequency;
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_L, &index);
	freq_hi = dbs_info->freq_table[index].frequency;

	/* Find out how long we have to be in hi and lo freqs */
	if (freq_hi == freq_lo) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_lo;
	}
	jiffies_total = usecs_to_jiffies(tea_tuners->sampling_rate);
	jiffies_hi = (freq_avg - freq_lo) * jiffies_total;
	jiffies_hi += ((freq_hi - freq_lo) / 1);
	jiffies_hi /= (freq_hi - freq_lo);
	jiffies_lo = jiffies_total - jiffies_hi;
	dbs_info->freq_lo = freq_lo;
	dbs_info->freq_lo_jiffies = jiffies_lo;
	dbs_info->freq_hi_jiffies = jiffies_hi;
	return freq_hi;
}

static void sad_tea_powersave_bias_init(void)
{
	int i;
	for_each_online_cpu(i) {
		sad_tea_powersave_bias_init_cpu(i);
	}
}

static void dbs_freq_increase(struct cpufreq_policy *policy, unsigned int freq)
{
	struct dbs_data *dbs_data = policy->governor_data;
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;

	if (tea_tuners->powersave_bias)
		freq = tea_ops.powersave_bias_target(policy, freq,
				CPUFREQ_RELATION_H);
	else if (policy->cur == policy->max)
		return;

	__cpufreq_driver_target(policy, freq, tea_tuners->powersave_bias ?
			CPUFREQ_RELATION_L : CPUFREQ_RELATION_H);
}

/*
 * Every sampling_rate, we check, if current idle time is less than 20%
 * (default), then we try to increase frequency. Else, we adjust the frequency
 * proportional to load.
 */
static void tea_check_cpu(int cpu, unsigned int load)
{
	struct tea_cpu_dbs_info_s *dbs_info = &per_cpu(tea_cpu_dbs_info, cpu);
	struct cpufreq_policy *policy = dbs_info->cdbs.cur_policy;
	struct dbs_data *dbs_data = policy->governor_data;
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;

	dbs_info->freq_lo = 0;

	/* Check for frequency increase */
	if (load > tea_tuners->up_threshold) {
		/* If switching to max speed, apply sampling_down_factor */
		if (policy->cur < policy->max)
			dbs_info->rate_mult =
				tea_tuners->sampling_down_factor;
		dbs_freq_increase(policy, policy->max);
	} else {
		/* Calculate the next frequency proportional to load */
		unsigned int freq_next, min_f, max_f;

		min_f = policy->cpuinfo.min_freq;
		max_f = policy->cpuinfo.max_freq;
		freq_next = min_f + load * (max_f - min_f) / 50;

		/* No longer fully busy, reset rate_mult */
		dbs_info->rate_mult = 1;

		if (!tea_tuners->powersave_bias) {
			__cpufreq_driver_target(policy, freq_next,
					CPUFREQ_RELATION_C);
			return;
		}

		freq_next = tea_ops.powersave_bias_target(policy, freq_next,
					CPUFREQ_RELATION_L);
		__cpufreq_driver_target(policy, freq_next, CPUFREQ_RELATION_C);
	}
}

static void tea_dbs_timer(struct work_struct *work)
{
	struct tea_cpu_dbs_info_s *dbs_info =
		container_of(work, struct tea_cpu_dbs_info_s, cdbs.work.work);
	unsigned int cpu = dbs_info->cdbs.cur_policy->cpu;
	struct tea_cpu_dbs_info_s *core_dbs_info = &per_cpu(tea_cpu_dbs_info,
			cpu);
	struct dbs_data *dbs_data = dbs_info->cdbs.cur_policy->governor_data;
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	int delay = 0, sample_type = core_dbs_info->sample_type;
	bool modify_all = true;

	mutex_lock(&core_dbs_info->cdbs.timer_mutex);
	if (!need_load_eval(&core_dbs_info->cdbs, tea_tuners->sampling_rate)) {
		modify_all = false;
		goto max_delay;
	}

	/* Common NORMAL_SAMPLE setup */
	core_dbs_info->sample_type = TEA_NORMAL_SAMPLE;
	if (sample_type == TEA_SUB_SAMPLE) {
		delay = core_dbs_info->freq_lo_jiffies;
		__cpufreq_driver_target(core_dbs_info->cdbs.cur_policy,
				core_dbs_info->freq_lo, CPUFREQ_RELATION_H);
	} else {
		dbs_check_cpu(dbs_data, cpu);
		if (core_dbs_info->freq_lo) {
			/* Setup timer for SUB_SAMPLE */
			core_dbs_info->sample_type = TEA_SUB_SAMPLE;
			delay = core_dbs_info->freq_hi_jiffies;
		}
	}

max_delay:
	if (!delay)
		delay = delay_for_sampling_rate(tea_tuners->sampling_rate
				* core_dbs_info->rate_mult);

	gov_queue_work(dbs_data, dbs_info->cdbs.cur_policy, delay, modify_all);
	mutex_unlock(&core_dbs_info->cdbs.timer_mutex);
}

/************************** sysfs interface ************************/
static struct common_dbs_data tea_dbs_cdata;

/**
 * update_sampling_rate - update sampling rate effective immediately if needed.
 * @new_rate: new sampling rate
 *
 * If new rate is smaller than the old, simply updating
 * dbs_tuners_int.sampling_rate might not be appropriate. For example, if the
 * original sampling_rate was 1 second and the requested new sampling rate is 10
 * ms because the user needs immediate reaction from sad_tea governor, but not
 * sure if higher frequency will be required or not, then, the governor may
 * change the sampling rate too late; up to 1 second later. Thus, if we are
 * reducing the sampling rate, we need to make the new value effective
 * immediately.
 */
static void update_sampling_rate(struct dbs_data *dbs_data,
		unsigned int new_rate)
{
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	int cpu;

	tea_tuners->sampling_rate = new_rate = max(new_rate,
			dbs_data->min_sampling_rate);

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct tea_cpu_dbs_info_s *dbs_info;
		unsigned long next_sampling, appointed_at;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (policy->governor != &cpufreq_gov_sad_tea) {
			cpufreq_cpu_put(policy);
			continue;
		}
		dbs_info = &per_cpu(tea_cpu_dbs_info, cpu);
		cpufreq_cpu_put(policy);

		mutex_lock(&dbs_info->cdbs.timer_mutex);

		if (!delayed_work_pending(&dbs_info->cdbs.work)) {
			mutex_unlock(&dbs_info->cdbs.timer_mutex);
			continue;
		}

		next_sampling = jiffies + usecs_to_jiffies(new_rate);
		appointed_at = dbs_info->cdbs.work.timer.expires;

		if (time_before(next_sampling, appointed_at)) {

			mutex_unlock(&dbs_info->cdbs.timer_mutex);
			cancel_delayed_work_sync(&dbs_info->cdbs.work);
			mutex_lock(&dbs_info->cdbs.timer_mutex);

			gov_queue_work(dbs_data, dbs_info->cdbs.cur_policy,
					usecs_to_jiffies(new_rate), true);

		}
		mutex_unlock(&dbs_info->cdbs.timer_mutex);
	}
}

static ssize_t store_sampling_rate(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	update_sampling_rate(dbs_data, input);
	return count;
}

static ssize_t store_io_is_busy(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	tea_tuners->io_is_busy = !!input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct tea_cpu_dbs_info_s *dbs_info = &per_cpu(tea_cpu_dbs_info,
									j);
		dbs_info->cdbs.prev_cpu_idle = get_cpu_idle_time(j,
			&dbs_info->cdbs.prev_cpu_wall, tea_tuners->io_is_busy);
	}
	return count;
}

static ssize_t store_up_threshold(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
			input < MIN_FREQUENCY_UP_THRESHOLD) {
		return -EINVAL;
	}

	tea_tuners->up_threshold = input;
	return count;
}

static ssize_t store_sampling_down_factor(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	unsigned int input, j;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;
	tea_tuners->sampling_down_factor = input;

	/* Reset down sampling multiplier in case it was active */
	for_each_online_cpu(j) {
		struct tea_cpu_dbs_info_s *dbs_info = &per_cpu(tea_cpu_dbs_info,
				j);
		dbs_info->rate_mult = 1;
	}
	return count;
}

static ssize_t store_ignore_nice_load(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == tea_tuners->ignore_nice_load) { /* nothing to do */
		return count;
	}
	tea_tuners->ignore_nice_load = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct tea_cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(tea_cpu_dbs_info, j);
		dbs_info->cdbs.prev_cpu_idle = get_cpu_idle_time(j,
			&dbs_info->cdbs.prev_cpu_wall, tea_tuners->io_is_busy);
		if (tea_tuners->ignore_nice_load)
			dbs_info->cdbs.prev_cpu_nice =
				kcpustat_cpu(j).cpustat[CPUTIME_NICE];

	}
	return count;
}

static ssize_t store_powersave_bias(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct tea_dbs_tuners *tea_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 500)
		input = 500;

	tea_tuners->powersave_bias = input;
	sad_tea_powersave_bias_init();
	return count;
}

show_store_one(tea, sampling_rate);
show_store_one(tea, io_is_busy);
show_store_one(tea, up_threshold);
show_store_one(tea, sampling_down_factor);
show_store_one(tea, ignore_nice_load);
show_store_one(tea, powersave_bias);
declare_show_sampling_rate_min(tea);

gov_sys_pol_attr_rw(sampling_rate);
gov_sys_pol_attr_rw(io_is_busy);
gov_sys_pol_attr_rw(up_threshold);
gov_sys_pol_attr_rw(sampling_down_factor);
gov_sys_pol_attr_rw(ignore_nice_load);
gov_sys_pol_attr_rw(powersave_bias);
gov_sys_pol_attr_ro(sampling_rate_min);

static struct attribute *dbs_attributes_gov_sys[] = {
	&sampling_rate_min_gov_sys.attr,
	&sampling_rate_gov_sys.attr,
	&up_threshold_gov_sys.attr,
	&sampling_down_factor_gov_sys.attr,
	&ignore_nice_load_gov_sys.attr,
	&powersave_bias_gov_sys.attr,
	&io_is_busy_gov_sys.attr,
	NULL
};

static struct attribute_group tea_attr_group_gov_sys = {
	.attrs = dbs_attributes_gov_sys,
	.name = "sad_tea",
};

static struct attribute *dbs_attributes_gov_pol[] = {
	&sampling_rate_min_gov_pol.attr,
	&sampling_rate_gov_pol.attr,
	&up_threshold_gov_pol.attr,
	&sampling_down_factor_gov_pol.attr,
	&ignore_nice_load_gov_pol.attr,
	&powersave_bias_gov_pol.attr,
	&io_is_busy_gov_pol.attr,
	NULL
};

static struct attribute_group tea_attr_group_gov_pol = {
	.attrs = dbs_attributes_gov_pol,
	.name = "sad_tea",
};

/************************** sysfs end ************************/

static int tea_init(struct dbs_data *dbs_data)
{
	struct tea_dbs_tuners *tuners;
	u64 idle_time;
	int cpu;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners) {
		pr_err("%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	cpu = get_cpu();
	idle_time = get_cpu_idle_time_us(cpu, NULL);
	put_cpu();
	if (idle_time != -1ULL) {
		/* Idle micro accounting is supported. Use finer thresholds */
		tuners->up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
		/*
		 * In nohz/micro accounting case we set the minimum frequency
		 * not depending on HZ, but fixed (very low). The deferred
		 * timer might skip some samples if idle/sleeping as needed.
		*/
		dbs_data->min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
	} else {
		tuners->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;

		/* For correct statistics, we need 10 ticks for each measure */
		dbs_data->min_sampling_rate = MSRR *
			jiffies_to_usecs(5);
	}

	tuners->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;
	tuners->ignore_nice_load = 0;
	tuners->powersave_bias = default_powersave_bias;
	

	dbs_data->tuners = tuners;
	mutex_init(&dbs_data->mutex);
	return 0;
}

static void tea_exit(struct dbs_data *dbs_data)
{
	kfree(dbs_data->tuners);
}

define_get_cpu_dbs_routines(tea_cpu_dbs_info);

static struct tea_ops tea_ops = {
	.powersave_bias_init_cpu = sad_tea_powersave_bias_init_cpu,
	.powersave_bias_target = generic_powersave_bias_target,
	.freq_increase = dbs_freq_increase,
};

static struct common_dbs_data tea_dbs_cdata = {
	.governor = GOV_SAD_TEA,
	.attr_group_gov_sys = &tea_attr_group_gov_sys,
	.attr_group_gov_pol = &tea_attr_group_gov_pol,
	.get_cpu_cdbs = get_cpu_cdbs,
	.get_cpu_dbs_info_s = get_cpu_dbs_info_s,
	.gov_dbs_timer = tea_dbs_timer,
	.gov_check_cpu = tea_check_cpu,
	.gov_ops = &tea_ops,
	.init = tea_init,
	.exit = tea_exit,
};

static void tea_set_powersave_bias(unsigned int powersave_bias)
{
	struct cpufreq_policy *policy;
	struct dbs_data *dbs_data;
	struct tea_dbs_tuners *tea_tuners;
	unsigned int cpu;
	cpumask_t done;

	default_powersave_bias = powersave_bias;
	cpumask_clear(&done);

	get_online_cpus();
	for_each_online_cpu(cpu) {
		if (cpumask_test_cpu(cpu, &done))
			continue;

		policy = per_cpu(tea_cpu_dbs_info, cpu).cdbs.cur_policy;
		if (!policy)
			continue;

		cpumask_or(&done, &done, policy->cpus);

		if (policy->governor != &cpufreq_gov_sad_tea)
			continue;

		dbs_data = policy->governor_data;
		tea_tuners = dbs_data->tuners;
		tea_tuners->powersave_bias = default_powersave_bias;
	}
	put_online_cpus();
}

void tea_register_powersave_bias_handler(unsigned int (*f)
		(struct cpufreq_policy *, unsigned int, unsigned int),
		unsigned int powersave_bias)
{
	tea_ops.powersave_bias_target = f;
	tea_set_powersave_bias(powersave_bias);
}
EXPORT_SYMBOL_GPL(tea_register_powersave_bias_handler);

void tea_unregister_powersave_bias_handler(void)
{
	tea_ops.powersave_bias_target = generic_powersave_bias_target;
	tea_set_powersave_bias(0);
}
EXPORT_SYMBOL_GPL(tea_unregister_powersave_bias_handler);

static int tea_cpufreq_governor_dbs(struct cpufreq_policy *policy,
		unsigned int event)
{
	return cpufreq_governor_dbs(policy, &tea_dbs_cdata, event);
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SAD_TEA
static
#endif
struct cpufreq_governor cpufreq_gov_sad_tea = {
	.name			= "sad_tea",
	.governor		= tea_cpufreq_governor_dbs,
	.max_transition_latency	= TLL,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_sad_tea);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_sad_tea);
}

MODULE_AUTHOR("TEA <kmd36rk@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_sad_tea' - A dynamic cpufreq governor for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SAD_TEA
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
 
