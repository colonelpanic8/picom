// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "compiler.h"
#include "transition.h"
#include "utils.h"

double animatable_get_progress(const struct animatable *a) {
	if (a->duration > 0) {
		return a->elapsed / a->duration;
	}
	return 1;
}

/// Get the current value of an `animatable`.
double animatable_get(const struct animatable *a) {
	if (a->duration > 0) {
		assert(a->elapsed < a->duration);
		double t = a->curve->sample(a->curve, animatable_get_progress(a));
		return (1 - t) * a->start + t * a->target;
	}
	return a->target;
}

/// Advance the animation by a given number of steps.
void animatable_advance(struct animatable *a, double elapsed) {
	if (a->duration == 0 || elapsed <= 0) {
		return;
	}

	assert(a->elapsed < a->duration);
	if (elapsed >= a->duration - a->elapsed) {
		a->elapsed = a->duration;
	} else {
		a->elapsed += elapsed;
	}

	if (a->elapsed == a->duration) {
		a->start = a->target;
		a->duration = 0;
		a->elapsed = 0;
		a->curve->free(a->curve);
		a->curve = NULL;
		if (a->callback) {
			a->callback(TRANSITION_COMPLETED, a->callback_data);
			a->callback = NULL;
			a->callback_data = NULL;
		}
	}
}

/// Returns whether an `animatable` is currently animating.
bool animatable_is_animating(const struct animatable *a) {
	assert(a->duration == 0 || a->elapsed < a->duration);
	return a->duration != 0;
}

/// Cancel the current animation of an `animatable`. This stops the animation and
/// the `animatable` will retain its current value.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_interrupt(struct animatable *a) {
	if (a->duration == 0) {
		return false;
	}

	a->start = animatable_get(a);
	a->target = a->start;
	a->duration = 0;
	a->elapsed = 0;
	a->curve->free(a->curve);
	a->curve = NULL;
	if (a->callback) {
		a->callback(TRANSITION_INTERRUPTED, a->callback_data);
		a->callback = NULL;
		a->callback_data = NULL;
	}
	return true;
}

/// Cancel the current animation of an `animatable` and set its value to its target.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_skip(struct animatable *a) {
	if (a->duration == 0) {
		return false;
	}

	a->start = a->target;
	a->duration = 0;
	a->elapsed = 0;
	a->curve->free(a->curve);
	a->curve = NULL;
	if (a->callback) {
		a->callback(TRANSITION_SKIPPED, a->callback_data);
		a->callback = NULL;
		a->callback_data = NULL;
	}
	return true;
}

/// Change the target value of an `animatable`.
/// If the `animatable` is already animating, the animation will be canceled first.
bool animatable_set_target(struct animatable *a, double target, double duration,
                           const struct curve *curve, transition_callback_fn cb, void *data) {
	animatable_interrupt(a);
	if (duration == 0 || a->start == target) {
		a->start = target;
		a->target = target;
		curve->free(curve);
		return false;
	}

	a->target = target;
	a->duration = duration;
	a->elapsed = 0;
	a->callback = cb;
	a->callback_data = data;
	a->curve = curve;
	return true;
}

/// Create a new animatable.
struct animatable animatable_new(double value) {
	struct animatable ret = {
	    .start = value,
	    .target = value,
	    .duration = 0,
	    .elapsed = 0,
	};
	return ret;
}

static double curve_sample_linear(const struct curve *this attr_unused, double progress) {
	return progress;
}

static void noop_free(const struct curve *this attr_unused) {
}

static void trivial_free(const struct curve *this) {
	free((void *)this);
}

const struct curve *curve_new_linear(void) {
	static const struct curve ret = {
	    .sample = curve_sample_linear,
	    .free = noop_free,
	};
	return &ret;
}

/// Cubic bezier interpolator.
///
/// Stolen from servo:
/// https://searchfox.org/mozilla-central/rev/5da2d56d12/servo/components/style/bezier.rs
struct cubic_bezier_curve {
	struct curve base;
	double ax, bx, cx;
	double ay, by, cy;
};

static inline double cubic_bezier_sample_x(const struct cubic_bezier_curve *self, double t) {
	return ((self->ax * t + self->bx) * t + self->cx) * t;
}

static inline double cubic_bezier_sample_y(const struct cubic_bezier_curve *self, double t) {
	return ((self->ay * t + self->by) * t + self->cy) * t;
}

static inline double
cubic_bezier_sample_derivative_x(const struct cubic_bezier_curve *self, double t) {
	return (3.0 * self->ax * t + 2.0 * self->bx) * t + self->cx;
}

// Solve for the `t` in cubic bezier function that corresponds to `x`
static inline double cubic_bezier_solve_x(const struct cubic_bezier_curve *this, double x) {
	static const int NEWTON_METHOD_ITERATIONS = 8;
	double t = x;
	// Fast path: try Newton's method.
	for (int i = 0; i < NEWTON_METHOD_ITERATIONS; i++) {
		double x2 = cubic_bezier_sample_x(this, t);
		if (fabs(x2 - x) < 1e-7) {
			return t;
		}
		double dx = cubic_bezier_sample_derivative_x(this, t);
		if (fabs(dx) < 1e-6) {
			break;
		}
		t -= (x2 - x) / dx;
	}

	// Slow path: Use bisection.
	double low = 0.0, high = 1.0;
	t = x;
	while (high - low > 1e-7) {
		double x2 = cubic_bezier_sample_x(this, t);
		if (fabs(x2 - x) < 1e-7) {
			return t;
		}
		if (x > x2) {
			low = t;
		} else {
			high = t;
		}
		t = (high - low) / 2.0 + low;
	}
	return t;
}

static double curve_sample_cubic_bezier(const struct curve *base, double progress) {
	auto this = (struct cubic_bezier_curve *)base;
	assert(progress >= 0 && progress <= 1);
	if (progress == 0 || progress == 1) {
		return progress;
	}
	double t = cubic_bezier_solve_x(this, progress);
	return cubic_bezier_sample_y(this, t);
}

const struct curve *curve_new_cubic_bezier(double x1, double y1, double x2, double y2) {
	if (x1 == y1 && x2 == y2) {
		return curve_new_linear();
	}

	assert(x1 >= 0 && x1 <= 1 && x2 >= 0 && x2 <= 1);
	auto ret = ccalloc(1, struct cubic_bezier_curve);
	ret->base.sample = curve_sample_cubic_bezier;
	ret->base.free = trivial_free;

	double cx = 3. * x1;
	double bx = 3. * (x2 - x1) - cx;
	double cy = 3. * y1;
	double by = 3. * (y2 - y1) - cy;
	ret->ax = 1. - cx - bx;
	ret->bx = bx;
	ret->cx = cx;
	ret->ay = 1. - cy - by;
	ret->by = by;
	ret->cy = cy;
	return &ret->base;
}

struct step_curve {
	struct curve base;
	int steps;
	bool jump_start, jump_end;
};

static double curve_sample_step(const struct curve *base, double progress) {
	auto this = (struct step_curve *)base;
	double y_steps = this->steps - 1 + this->jump_end + this->jump_start,
	       x_steps = this->steps;
	if (progress == 1) {
		return 1;
	}
	if (progress == 0) {
		return this->jump_start ? 1 / y_steps : 0;
	}

	double scaled = progress * x_steps;
	double quantized = this->jump_start ? ceil(scaled) : floor(scaled);
	return quantized / y_steps;
}

const struct curve *curve_new_step(int steps, bool jump_start, bool jump_end) {
	assert(steps > 0);
	auto ret = ccalloc(1, struct step_curve);
	ret->base.sample = curve_sample_step;
	ret->base.free = trivial_free;
	ret->steps = steps;
	ret->jump_start = jump_start;
	ret->jump_end = jump_end;
	return &ret->base;
}
