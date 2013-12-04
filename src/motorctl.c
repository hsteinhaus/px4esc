/****************************************************************************
 *
 *   Copyright (C) 2013 PX4 Development Team. All rights reserved.
 *   Author: Pavel Kirienko <pavel.kirienko@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <math.h>
#include <ch.h>
#include "motorctl.h"
#include "motor/motor.h"
#include "motor/timer.h"

#define CONTROL_PERIOD_USEC     1000

#define MIN_VALID_INPUT_VOLTAGE 4.0
#define MAX_VALID_INPUT_VOLTAGE 40.0


static unsigned comm_period_to_rpm(uint32_t comm_period);


static Mutex _mutex;
static EVENTSOURCE_DECL(_setpoint_update_event);
static WORKING_AREA(_wa_control_thread, 1024);

/*
 * TODO: setpoint update timeout
 * TODO: check the voltage/current readings before starting
 */
static struct state
{
	enum motorctl_mode mode;
	int limit_mask;

	float dc_actual;
	float dc_openloop_setpoint;

	unsigned rpm_setpoint;

	float input_voltage;
	float input_current;
} _state;

static struct params
{
	float spinup_voltage;

	float dc_step_max;
	float dc_slope;

	float voltage_current_lowpass_tau;

	int poles;
	bool reverse;

	uint32_t comm_period_limit;
	unsigned rpm_max;
	unsigned rpm_min;
} _params;


static void configure(void)
{
	// TODO: configs
	_params.spinup_voltage = 2.0;

	_params.dc_step_max = 0.2;
	_params.dc_slope = 1.0;

	_params.voltage_current_lowpass_tau = 1 / 0.5;

	_params.poles = 14;
	_params.reverse = false;

	_params.comm_period_limit = motor_get_limit_comm_period_hnsec();
	_params.rpm_max = comm_period_to_rpm(_params.comm_period_limit);
	_params.rpm_min = 500;

	lowsyslog("Motor control: RPM range: [%u, %u]; poles: %i\n", _params.rpm_min, _params.rpm_max, _params.poles);
}

static float lowpass(float xold, float xnew, float tau, float dt)
{
	return (dt * xnew + tau * xold) / (dt + tau);
}

static void init_filters(void)
{
	motor_get_input_voltage_current(&_state.input_voltage, &_state.input_current);
}

static void update_filters(float dt)
{
	float voltage = 0, current = 0;
	motor_get_input_voltage_current(&voltage, &current);

	_state.input_voltage = lowpass(_state.input_voltage, voltage, _params.voltage_current_lowpass_tau, dt);
	_state.input_current = lowpass(_state.input_current, current, _params.voltage_current_lowpass_tau, dt);
}

static void stop(void)
{
	motor_stop();
	_state.limit_mask = 0;
	_state.dc_actual = 0.0;
	_state.dc_openloop_setpoint = 0.0;
	_state.rpm_setpoint = 0;
}

static void update_control_non_running(void)
{
	// Do not change anything while the motor is starting
	const enum motor_state motor_state = motor_get_state();
	if (motor_state == MOTOR_STATE_STARTING)
		return;

	// Start if necessary
	const float spinup_dc = _params.spinup_voltage / _state.input_voltage;
	_state.dc_actual = spinup_dc;
	_state.limit_mask = 0;

	const bool need_start =
		(_state.mode == MOTORCTL_MODE_OPENLOOP && (_state.dc_openloop_setpoint >= spinup_dc)) ||
		(_state.mode == MOTORCTL_MODE_RPM && (_state.rpm_setpoint >= _params.rpm_min));

	if (need_start)
		motor_start(spinup_dc, spinup_dc, _params.reverse);
}

static float update_control_open_loop(void)
{
	const uint32_t cp = motor_get_comm_period_hnsec();
	if (cp == 0)      // The motor just stopped
		return nan("");

	if (cp < _params.comm_period_limit) {
		// Simple P controller
		const float c1 = _params.comm_period_limit;
		const float c0 = _params.comm_period_limit / 2;
		const float dc = (cp - c0) / (c1 - c0);

		if (dc < _state.dc_openloop_setpoint) {
			_state.limit_mask |= MOTORCTL_LIMIT_RPM;
			return dc;
		}
	}
	_state.limit_mask &= ~MOTORCTL_LIMIT_RPM;
	if (_state.dc_openloop_setpoint > 0.0)
		return _state.dc_openloop_setpoint;
	else
		return nan("");
}

static float update_control_rpm(float dt)
{
	return nan(""); // TODO
}

static void update_control(float dt)
{
	const enum motor_state motor_state = motor_get_state();
	if (motor_state != MOTOR_STATE_RUNNING) {
		update_control_non_running();
		return;
	}

	/*
	 * Primary control logic; can return NAN to stop the motor
	 */
	float new_duty_cycle = nan("");
	if (_state.mode == MOTORCTL_MODE_OPENLOOP) {
		new_duty_cycle = update_control_open_loop();
	}
	else if (_state.mode == MOTORCTL_MODE_RPM) {
		new_duty_cycle = update_control_rpm(dt);
	}
	else assert(0);

	if (!isfinite(new_duty_cycle)) {
		stop();
		return;
	}

	/*
	 * Duty cycle slope control
	 */
	if (fabs(new_duty_cycle - _state.dc_actual) > _params.dc_step_max) {
		float step = _params.dc_slope * dt;
		if (new_duty_cycle < _state.dc_actual)
			step = -step;

		new_duty_cycle = _state.dc_actual + step;
		_state.limit_mask |= MOTORCTL_LIMIT_ACCEL;
	}
	else {
		_state.limit_mask &= ~MOTORCTL_LIMIT_ACCEL;
	}

	/*
	 * Update
	 */
	_state.dc_actual = new_duty_cycle;
	motor_set_duty_cycle(_state.dc_actual);
}

static msg_t control_thread(void* arg)
{
	EventListener listener;
	chEvtRegisterMask(&_setpoint_update_event, &listener, ALL_EVENTS);

	uint64_t timestamp_hnsec = motor_timer_hnsec();

	while (1) {
		/*
		 * Make sure the event is set when the mutex is unlocked.
		 * Otherwise this thread will take control, stumble upon the locked mutex, return the control
		 * to the thread that holds the mutex, unlock the mutex, then proceed.
		 */
		chEvtWaitAnyTimeout(ALL_EVENTS, US2ST(CONTROL_PERIOD_USEC));

		chMtxLock(&_mutex);

		const uint64_t new_timestamp_hnsec = motor_timer_hnsec();
		const float dt = (new_timestamp_hnsec - timestamp_hnsec) / (float)HNSEC_PER_SEC;
		timestamp_hnsec = new_timestamp_hnsec;

		update_filters(dt);
		update_control(dt);

		chMtxUnlock();
	}

	chEvtUnregister(&_setpoint_update_event, &listener);
	return 0;
}

int motorctl_init(void)
{
	int ret = motor_init();
	if (ret)
		return ret;

	chMtxInit(&_mutex);
	chEvtInit(&_setpoint_update_event);

	configure();

	init_filters();
	if (_state.input_voltage < MIN_VALID_INPUT_VOLTAGE || _state.input_voltage > MAX_VALID_INPUT_VOLTAGE) {
		lowsyslog("Motor control: invalid input voltage: %f\n", _state.input_voltage);
		return -1;
	}

	assert_always(chThdCreateStatic(_wa_control_thread, sizeof(_wa_control_thread),
		HIGHPRIO, control_thread, NULL));

	return 0;
}

void motorctl_set_duty_cycle(float dc)
{
	chMtxLock(&_mutex);

	_state.mode = MOTORCTL_MODE_OPENLOOP;

	if (dc < 0.0) dc = 0.0;
	if (dc > 1.0) dc = 1.0;
	_state.dc_openloop_setpoint = dc;

	chMtxUnlock();

	// Wake the control thread to process the new setpoint immediately
	chEvtBroadcastFlags(&_setpoint_update_event, ALL_EVENTS);
}

void motorctl_set_rpm(unsigned rpm)
{
	chMtxLock(&_mutex);

	_state.mode = MOTORCTL_MODE_RPM;

	if (rpm > _params.rpm_max)
		rpm = _params.rpm_max;
	_state.rpm_setpoint = rpm;

	chMtxUnlock();

	// Wake the control thread to process the new setpoint immediately
	chEvtBroadcastFlags(&_setpoint_update_event, ALL_EVENTS);
}

float motorctl_get_duty_cycle(void)
{
	chMtxLock(&_mutex);
	float ret = _state.dc_actual;
	chMtxUnlock();
	return ret;
}

unsigned motorctl_get_rpm(void)
{
	chMtxLock(&_mutex);
	uint32_t cp = motor_get_comm_period_hnsec();
	unsigned rpm = comm_period_to_rpm(cp);
	chMtxUnlock();
	return rpm;
}

enum motorctl_mode motorctl_get_mode(void)
{
	chMtxLock(&_mutex);
	enum motorctl_mode ret = _state.mode;
	chMtxUnlock();
	return ret;
}

bool motorctl_is_running(void)
{
	chMtxLock(&_mutex);
	bool ret = motor_get_state() != MOTOR_STATE_IDLE;
	chMtxUnlock();
	return ret;
}

int motorctl_get_limit_mask(void)
{
	chMtxLock(&_mutex);
	int ret = _state.limit_mask;
	chMtxUnlock();
	return ret;
}

void motorctl_get_input_voltage_current(float* out_voltage, float* out_current)
{
	chMtxLock(&_mutex);

	if (out_voltage)
		*out_voltage = _state.input_voltage;
	if (out_current)
		*out_current = _state.input_current;

	chMtxUnlock();
}

// erpm_to_comm_period = lambda erpm: ((10000000 * 60) / erpm) / 6
// comm_period_to_erpm = lambda cp: (10000000 * 60) / (cp * 6)

static unsigned comm_period_to_rpm(uint32_t comm_period_hnsec)
{
	assert(_params.poles > 0);
	const uint32_t x = (120ULL * (uint64_t)HNSEC_PER_SEC) / (_params.poles * 6);
	return x / comm_period_hnsec;
}
