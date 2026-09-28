/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe_value.h>

LOG_MODULE_REGISTER(mpipe_value, CONFIG_MPIPE_LOG_LEVEL);

#define MPIPE_VALUE_PRIMITIVE_MASK                                                                 \
	(BIT(MPIPE_TYPE_BOOLEAN) | BIT(MPIPE_TYPE_INT) | BIT(MPIPE_TYPE_UINT))

static const uint32_t mpipe_value_intersect_mask[MPIPE_TYPE_COUNT] = {
	[MPIPE_TYPE_NONE] = 0,
	[MPIPE_TYPE_BOOLEAN] = BIT(MPIPE_TYPE_BOOLEAN),
	[MPIPE_TYPE_INT] = BIT(MPIPE_TYPE_INT) | BIT(MPIPE_TYPE_INT_RANGE),
	[MPIPE_TYPE_UINT] = BIT(MPIPE_TYPE_UINT) | BIT(MPIPE_TYPE_UINT_RANGE),
	[MPIPE_TYPE_INT_RANGE] = BIT(MPIPE_TYPE_INT) | BIT(MPIPE_TYPE_INT_RANGE),
	[MPIPE_TYPE_UINT_RANGE] = BIT(MPIPE_TYPE_UINT) | BIT(MPIPE_TYPE_UINT_RANGE),
};

bool mpipe_value_is_primitive(const struct mpipe_value *value)
{
	__ASSERT_NO_MSG(value != NULL);

	if (!IN_RANGE(value->type, MPIPE_TYPE_NONE + 1, MPIPE_TYPE_COUNT - 1)) {
		return false;
	}

	return (MPIPE_VALUE_PRIMITIVE_MASK & BIT(value->type)) != 0;
}

static void mpipe_value_set_range(struct mpipe_value *value, enum mpipe_value_type type,
				  va_list *args)
{
	/* A signed range is passed as int, an unsigned one as uint32_t */
	if (type == MPIPE_TYPE_INT_RANGE) {
		value->range.min.v_int = va_arg(*args, int);
		value->range.max.v_int = va_arg(*args, int);
		value->range.step.v_int = va_arg(*args, int);
	} else {
		value->range.min.v_uint = va_arg(*args, uint32_t);
		value->range.max.v_uint = va_arg(*args, uint32_t);
		value->range.step.v_uint = va_arg(*args, uint32_t);
	}
}

int mpipe_value_set_va_list(struct mpipe_value *value, enum mpipe_value_type type, va_list *args)
{
	__ASSERT_NO_MSG(args != NULL);

	__ASSERT_NO_MSG(value != NULL);

	/* Any integer type narrower than int arrives as int through the variadic argument list */
	switch (type) {
	case MPIPE_TYPE_BOOLEAN:
		value->v_boolean = (va_arg(*args, int) != 0);
		break;
	case MPIPE_TYPE_INT:
		value->v_int = va_arg(*args, int);
		break;
	case MPIPE_TYPE_UINT:
		value->v_uint = va_arg(*args, uint32_t);
		break;
	case MPIPE_TYPE_INT_RANGE:
	case MPIPE_TYPE_UINT_RANGE:
		mpipe_value_set_range(value, type, args);
		break;
	default:
		LOG_ERR("Unknown mpipe_value type: %d", type);
		return -EINVAL;
	}

	/* Stamp the type last so a rejected one leaves the value untouched */
	value->type = type;

	return 0;
}

int mpipe_value_set(struct mpipe_value *value, int type, ...)
{
	int ret;
	va_list args;

	va_start(args, type);
	ret = mpipe_value_set_va_list(value, type, &args);
	va_end(args);

	return ret;
}

int32_t mpipe_value_get_int(const struct mpipe_value *value)
{
	__ASSERT_NO_MSG(value != NULL);

	return value->v_int;
}

uint32_t mpipe_value_get_uint(const struct mpipe_value *value)
{
	__ASSERT_NO_MSG(value != NULL);

	return value->v_uint;
}

bool mpipe_value_get_boolean(const struct mpipe_value *value)
{
	__ASSERT_NO_MSG(value != NULL);

	return value->v_boolean;
}

int32_t mpipe_value_get_int_range_min(const struct mpipe_value *range)
{
	__ASSERT_NO_MSG(range != NULL);

	return range->range.min.v_int;
}

int32_t mpipe_value_get_int_range_max(const struct mpipe_value *range)
{
	__ASSERT_NO_MSG(range != NULL);

	return range->range.max.v_int;
}

int32_t mpipe_value_get_int_range_step(const struct mpipe_value *range)
{
	__ASSERT_NO_MSG(range != NULL);

	return range->range.step.v_int;
}

uint32_t mpipe_value_get_uint_range_min(const struct mpipe_value *range)
{
	__ASSERT_NO_MSG(range != NULL);

	return range->range.min.v_uint;
}

uint32_t mpipe_value_get_uint_range_max(const struct mpipe_value *range)
{
	__ASSERT_NO_MSG(range != NULL);

	return range->range.max.v_uint;
}

uint32_t mpipe_value_get_uint_range_step(const struct mpipe_value *range)
{
	__ASSERT_NO_MSG(range != NULL);

	return range->range.step.v_uint;
}

static bool mpipe_value_primitive_equal(const struct mpipe_value *val1,
					const struct mpipe_value *val2)
{
	switch (val1->type) {
	case MPIPE_TYPE_BOOLEAN:
		return val1->v_boolean == val2->v_boolean;
	case MPIPE_TYPE_INT:
		return val1->v_int == val2->v_int;
	case MPIPE_TYPE_UINT:
		return val1->v_uint == val2->v_uint;
	default:
		return false;
	}
}

/* A range holds min + n * step up to max; a step of 0 counts as 1 */
static bool mpipe_value_grid_holds(int64_t min, int64_t max, int64_t step, int64_t v)
{
	step = MAX(step, 1);

	return v >= min && v <= max && ((v - min) % step) == 0;
}

/*
 * Intersect two ranges as sets of points: the first value on both grids up to
 * the last one, with the least common multiple of the steps between them.
 */
static int mpipe_value_intersect_grids(int64_t min1, int64_t max1, int64_t step1, int64_t min2,
				       int64_t max2, int64_t step2, int64_t *min, int64_t *max,
				       int64_t *step)
{
	int64_t lo = MAX(min1, min2);
	int64_t hi = MIN(max1, max2);
	uint64_t lcm;
	int64_t v;
	bool found = false;

	step1 = MAX(step1, 1);
	step2 = MAX(step2, 1);

	if (lo > hi) {
		return -ENOENT;
	}

	/* Walk the first grid from lo; after step2 points every residue has been tried */
	v = min1 + DIV_ROUND_UP(lo - min1, step1) * step1;
	for (int64_t i = 0; i < step2 && v <= hi; i++, v += step1) {
		if (((v - min2) % step2) == 0) {
			found = true;
			break;
		}
	}

	if (!found) {
		return -ENOENT;
	}

	lcm = sys_lcm_u((uint32_t)step1, (uint32_t)step2);
	*min = v;
	*max = (lcm > (uint64_t)(hi - v)) ? v : v + (int64_t)(((uint64_t)(hi - v) / lcm) * lcm);
	*step = (lcm > INT32_MAX) ? INT32_MAX : (int64_t)lcm;

	return 0;
}

static int mpipe_value_intersect_range(const struct mpipe_value *ref_val,
				       const struct mpipe_value *compare_val,
				       struct mpipe_value *out)
{
	int64_t min;
	int64_t max;
	int64_t step;
	int ret;

	if (ref_val->type == MPIPE_TYPE_INT_RANGE && compare_val->type == MPIPE_TYPE_INT_RANGE) {
		ret = mpipe_value_intersect_grids(
			ref_val->range.min.v_int, ref_val->range.max.v_int,
			ref_val->range.step.v_int, compare_val->range.min.v_int,
			compare_val->range.max.v_int, compare_val->range.step.v_int, &min, &max,
			&step);
		if (ret != 0) {
			return ret;
		}

		out->type = MPIPE_TYPE_INT_RANGE;
		out->range.min.v_int = (int32_t)min;
		out->range.max.v_int = (int32_t)max;
		out->range.step.v_int = (int32_t)step;

		return 0;
	}

	if (ref_val->type == MPIPE_TYPE_UINT_RANGE && compare_val->type == MPIPE_TYPE_UINT_RANGE) {
		ret = mpipe_value_intersect_grids(
			ref_val->range.min.v_uint, ref_val->range.max.v_uint,
			ref_val->range.step.v_uint, compare_val->range.min.v_uint,
			compare_val->range.max.v_uint, compare_val->range.step.v_uint, &min, &max,
			&step);
		if (ret != 0) {
			return ret;
		}

		out->type = MPIPE_TYPE_UINT_RANGE;
		out->range.min.v_uint = (uint32_t)min;
		out->range.max.v_uint = (uint32_t)max;
		out->range.step.v_uint = (uint32_t)step;

		return 0;
	}

	if ((ref_val->type == MPIPE_TYPE_INT_RANGE && compare_val->type == MPIPE_TYPE_INT &&
	     mpipe_value_grid_holds(ref_val->range.min.v_int, ref_val->range.max.v_int,
				    ref_val->range.step.v_int, compare_val->v_int)) ||
	    (ref_val->type == MPIPE_TYPE_UINT_RANGE && compare_val->type == MPIPE_TYPE_UINT &&
	     mpipe_value_grid_holds(ref_val->range.min.v_uint, ref_val->range.max.v_uint,
				    ref_val->range.step.v_uint, compare_val->v_uint))) {
		*out = *compare_val;
		return 0;
	}

	return -ENOENT;
}

int mpipe_value_intersect(const struct mpipe_value *val1, const struct mpipe_value *val2,
			  struct mpipe_value *out)
{
	const struct mpipe_value *ref_val, *compare_val;

	__ASSERT_NO_MSG(out != NULL);
	__ASSERT_NO_MSG(val1 != NULL);
	__ASSERT_NO_MSG(val2 != NULL);

	/* Only a pair of types the mask allows can have a common value */
	if (!IN_RANGE(val1->type, MPIPE_TYPE_NONE, MPIPE_TYPE_COUNT - 1) ||
	    !IN_RANGE(val2->type, MPIPE_TYPE_NONE, MPIPE_TYPE_COUNT - 1) ||
	    (mpipe_value_intersect_mask[val1->type] & BIT(val2->type)) == 0) {
		return -ENOENT;
	}

	/* A range type has a higher ordinal than the scalar it can contain */
	if (val1->type >= val2->type) {
		ref_val = val1;
		compare_val = val2;
	} else {
		ref_val = val2;
		compare_val = val1;
	}

	if (mpipe_value_is_primitive(ref_val)) {
		if (!mpipe_value_primitive_equal(val1, val2)) {
			return -ENOENT;
		}

		*out = *val1;

		return 0;
	}

	switch (ref_val->type) {
	case MPIPE_TYPE_INT_RANGE:
	case MPIPE_TYPE_UINT_RANGE:
		return mpipe_value_intersect_range(ref_val, compare_val, out);
	default:
		return -ENOENT;
	}
}

static inline void mpipe_value_print_boolean(const struct mpipe_value *value)
{
	printk("%s", value->v_boolean ? "true" : "false");
}

static inline void mpipe_value_print_int(const struct mpipe_value *value)
{
	printk("%d", value->v_int);
}

static inline void mpipe_value_print_uint(const struct mpipe_value *value)
{
	printk("%u", value->v_uint);
}

static inline void mpipe_value_print_int_range(const struct mpipe_value *value)
{
	printk("[%d, %d, %d]", value->range.min.v_int, value->range.max.v_int,
	       value->range.step.v_int);
}

static inline void mpipe_value_print_uint_range(const struct mpipe_value *value)
{
	printk("[%u, %u, %u]", value->range.min.v_uint, value->range.max.v_uint,
	       value->range.step.v_uint);
}

typedef void (*mpipe_value_print_fn)(const struct mpipe_value *);

static const mpipe_value_print_fn mpipe_value_print_table[MPIPE_TYPE_COUNT] = {
	[MPIPE_TYPE_NONE] = NULL,
	[MPIPE_TYPE_BOOLEAN] = mpipe_value_print_boolean,
	[MPIPE_TYPE_INT] = mpipe_value_print_int,
	[MPIPE_TYPE_UINT] = mpipe_value_print_uint,
	[MPIPE_TYPE_INT_RANGE] = mpipe_value_print_int_range,
	[MPIPE_TYPE_UINT_RANGE] = mpipe_value_print_uint_range,
};

void mpipe_value_print(const struct mpipe_value *value, bool new_line)
{
	__ASSERT_NO_MSG(value != NULL);

	if (value->type >= ARRAY_SIZE(mpipe_value_print_table) ||
	    mpipe_value_print_table[value->type] == NULL) {
		LOG_ERR("Invalid mpipe_value to print");
		return;
	}

	mpipe_value_print_table[value->type](value);

	if (new_line) {
		printk("\n");
	}
}
