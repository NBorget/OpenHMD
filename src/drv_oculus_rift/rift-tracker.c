/*
 * Rift position tracking
 * Copyright 2014-2015 Philipp Zabel
 * Copyright 2019 Jan Schmidt
 * SPDX-License-Identifier: BSL-1.0
 */

 #define _GNU_SOURCE

#include <libusb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "rift-tracker.h"
#include "rift-sensor.h"

#include "rift-sensor-maths.h"
#include "rift-sensor-opencv.h"
#include "rift-sensor-pose-helper.h"

#include "rift-debug-draw.h"

#include "ohmd-pipewire.h"

#define ASSERT_MSG(_v, label, ...) if(!(_v)){ fprintf(stderr, __VA_ARGS__); goto label; }

#define MAX_SENSORS 4

/* Number of state slots to use for quat/position updates */
#define NUM_POSE_DELAY_SLOTS 3

typedef struct rift_tracked_device_priv rift_tracked_device_priv;
typedef struct rift_tracker_pose_delay_slot rift_tracker_pose_delay_slot;

struct rift_tracker_pose_delay_slot {
	int slot_id;		/* Index of the slot */
	bool valid;			/* true if the exposure info was set */
	int use_count;	/* Number of frames using this slot */

	uint64_t device_time_ns; /* Device time this slot is currently tracking */
};

/* Internal full tracked device struct */
struct rift_tracked_device_priv {
	rift_tracked_device base;

	int index; /* Index of this entry in the devices array for the tracker and exposures */

	ohmd_mutex *device_lock;
	fusion simple_fusion;

	/* 6DOF Kalman Filter */
	rift_kalman_6dof_filter ukf_fusion;

	/* Account keeping for UKF fusion slots */
	int delay_slot_index;
	rift_tracker_pose_delay_slot delay_slots[NUM_POSE_DELAY_SLOTS];

	/* Transform from the fusion pose (which tracks the IMU, oriented to the screens/view)
	 * to the model the camera will see, which is offset and rotated 180 degrees */
	posef fusion_to_model;

	/* The model (HMD/controller) pose -> world transform */
	posef pose;

	uint32_t last_device_ts;
	uint64_t device_time_ns;

	int num_pending_imu_observations;
	rift_tracked_device_imu_observation pending_imu_observations[RIFT_MAX_PENDING_IMU_OBSERVATIONS];

	ohmd_pw_debug_stream *debug_metadata;
	FILE *debug_file;
};

struct rift_tracker_ctx_s
{
	ohmd_context* ohmd_ctx;
	libusb_context *usb_ctx;
	ohmd_mutex *tracker_lock;

	ohmd_thread* usb_thread;
	int usb_completed;

	bool have_exposure_info;
	rift_tracker_exposure_info exposure_info;

	rift_sensor_ctx *sensors[MAX_SENSORS];
	uint8_t n_sensors;

	rift_tracked_device_priv devices[RIFT_MAX_TRACKED_DEVICES];
	uint8_t n_devices;
};

static void rift_tracked_device_send_imu_debug(rift_tracked_device_priv *dev);
static void rift_tracked_device_update_exposure (rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info);
static void rift_tracked_device_frame_captured(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info);
static void rift_tracked_device_frame_released(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info);

rift_tracked_device *
rift_tracker_add_device (rift_tracker_ctx *ctx, int device_id, posef *imu_pose, rift_leds *leds)
{
	int i, s;
	rift_tracked_device_priv *next_dev;
	char device_name[64];

	snprintf(device_name,64,"openhmd-rift-device-%d", device_id);
	device_name[63] = 0;

	assert (ctx->n_devices < RIFT_MAX_TRACKED_DEVICES);

	ohmd_lock_mutex (ctx->tracker_lock);
	next_dev = ctx->devices + ctx->n_devices;

	next_dev->base.id = device_id;
	ofusion_init(&next_dev->simple_fusion);
	rift_kalman_6dof_init(&next_dev->ukf_fusion, NUM_POSE_DELAY_SLOTS);
	next_dev->device_time_ns = 0;

	/* Init delay slot bookkeeping */
	for (s = 0; s < NUM_POSE_DELAY_SLOTS; s++) {
		rift_tracker_pose_delay_slot *slot = next_dev->delay_slots + s;

		slot->slot_id = s;
		slot->valid = false;
	}

	next_dev->fusion_to_model = *imu_pose;

	next_dev->debug_metadata = ohmd_pw_debug_stream_new (device_name);
	next_dev->base.leds = leds;
	next_dev->base.led_search = led_search_model_new (leds);
	ctx->n_devices++;
	ohmd_unlock_mutex (ctx->tracker_lock);

	/* Tell the sensors about the new device */
	for (i = 0; i < ctx->n_sensors; i++) {
		rift_sensor_ctx *sensor_ctx = ctx->sensors[i];
		if (!rift_sensor_add_device (sensor_ctx, (rift_tracked_device *) next_dev)) {
			LOGE("Failed to configure object tracking for device %d\n", device_id);
		}
	}

	printf("device %d online. Now tracking.\n", device_id);
	return (rift_tracked_device *) next_dev;
}

static unsigned int uvc_handle_events(void *arg)
{
	rift_tracker_ctx *tracker_ctx = arg;

	while (!tracker_ctx->usb_completed)
		libusb_handle_events_completed(tracker_ctx->usb_ctx, &tracker_ctx->usb_completed);

	return 0;
}

rift_tracker_ctx *
rift_tracker_new (ohmd_context* ohmd_ctx,
		const uint8_t radio_id[5])
{
	rift_tracker_ctx *tracker_ctx = NULL;
	int ret, i;
	libusb_device **devs;

	tracker_ctx = ohmd_alloc(ohmd_ctx, sizeof (rift_tracker_ctx));
	tracker_ctx->ohmd_ctx = ohmd_ctx;
	tracker_ctx->tracker_lock = ohmd_create_mutex(ohmd_ctx);

	for (i = 0; i < RIFT_MAX_TRACKED_DEVICES; i++) {
		rift_tracked_device_priv *dev = tracker_ctx->devices + i;
		dev->index = i;
		dev->device_lock = ohmd_create_mutex(ohmd_ctx);
	}

	ret = libusb_init(&tracker_ctx->usb_ctx);
	ASSERT_MSG(ret >= 0, fail, "could not initialize libusb\n");

	ret = libusb_get_device_list(tracker_ctx->usb_ctx, &devs);
	ASSERT_MSG(ret >= 0, fail, "Could not get USB device list\n");

	/* Start USB event thread */
	tracker_ctx->usb_completed = false;
	tracker_ctx->usb_thread = ohmd_create_thread (ohmd_ctx, uvc_handle_events, tracker_ctx);

	for (i = 0; devs[i]; ++i) {
		struct libusb_device_descriptor desc;
		libusb_device_handle *usb_devh;
		rift_sensor_ctx *sensor_ctx = NULL;
		unsigned char serial[33];

		ret = libusb_get_device_descriptor(devs[i], &desc);
		if (ret < 0)
			continue; /* Can't access this device */
		if (desc.idVendor != 0x2833 || (desc.idProduct != CV1_PID && desc.idProduct != DK2_PID))
			continue;

		ret = libusb_open(devs[i], &usb_devh);
		if (ret) {
			fprintf (stderr, "Failed to open Rift Sensor device. Check permissions\n");
			continue;
		}

		sprintf ((char *) serial, "UNKNOWN");
		serial[32] = '\0';

		if (desc.iSerialNumber) {
			ret = libusb_get_string_descriptor_ascii(usb_devh, desc.iSerialNumber, serial, 32);
			if (ret < 0)
				fprintf (stderr, "Failed to read the Rift Sensor Serial number.\n");
		}

		sensor_ctx = rift_sensor_new (ohmd_ctx, tracker_ctx->n_sensors, (char *) serial, tracker_ctx->usb_ctx, usb_devh, tracker_ctx, radio_id);
		if (sensor_ctx != NULL) {
			tracker_ctx->sensors[tracker_ctx->n_sensors] = sensor_ctx;
			tracker_ctx->n_sensors++;
			if (tracker_ctx->n_sensors == MAX_SENSORS)
				break;
		}
	}
	libusb_free_device_list(devs, 1);

	printf ("Opened %u Rift Sensor cameras\n", tracker_ctx->n_sensors);

	return tracker_ctx;

fail:
	if (tracker_ctx)
		rift_tracker_free (tracker_ctx);
	return NULL;
}

bool rift_tracker_get_exposure_info (rift_tracker_ctx *ctx, rift_tracker_exposure_info *info)
{
	bool ret;

	ohmd_lock_mutex (ctx->tracker_lock);
	ret = ctx->have_exposure_info;
	*info = ctx->exposure_info;
	ohmd_unlock_mutex (ctx->tracker_lock);

	return ret;
}

void rift_tracker_update_exposure (rift_tracker_ctx *ctx, uint32_t hmd_ts, uint16_t exposure_count, uint32_t exposure_hmd_ts, uint8_t led_pattern_phase)
{
	bool exposure_changed = false;
	int i;

	ohmd_lock_mutex (ctx->tracker_lock);
	if (ctx->exposure_info.led_pattern_phase != led_pattern_phase) {
		LOGD ("%f LED pattern phase changed to %d",
			(double) (now) / 1000000.0, led_pattern_phase);
		ctx->exposure_info.led_pattern_phase = led_pattern_phase;
	}

	if (ctx->exposure_info.count != exposure_count) {
		uint64_t now = ohmd_monotonic_get(ctx->ohmd_ctx);

		exposure_changed = true;

		ctx->exposure_info.local_ts = now;
		ctx->exposure_info.count = exposure_count;
		ctx->exposure_info.hmd_ts = exposure_hmd_ts;
		ctx->exposure_info.led_pattern_phase = led_pattern_phase;
		ctx->have_exposure_info = true;

		LOGD ("%f Have new exposure TS %u count %d LED pattern phase %d",
			(double) (now) / 1000000.0, exposure_count, exposure_hmd_ts, led_pattern_phase);

		if ((int32_t)(exposure_hmd_ts - hmd_ts) < -1500) {
			LOGW("Exposure timestamp %u was more than 1.5 IMU samples earlier than IMU ts %u by %u µS",
					exposure_hmd_ts, hmd_ts, hmd_ts - exposure_hmd_ts);
		}

		ctx->exposure_info.n_devices = ctx->n_devices;

		for (i = 0; i < ctx->n_devices; i++) {
			rift_tracked_device_priv *dev = ctx->devices + i;
			rift_tracked_device_exposure_info *dev_info = ctx->exposure_info.devices + i;

			ohmd_lock_mutex (dev->device_lock);
			rift_tracked_device_update_exposure(dev, dev_info);

			rift_tracked_device_send_imu_debug(dev);
			ohmd_unlock_mutex (dev->device_lock);
		}
	}
	ohmd_unlock_mutex (ctx->tracker_lock);

	if (exposure_changed) {
		/* Tell sensors about the new exposure info, outside the lock to avoid
		 * deadlocks from callbacks */
		for (i = 0; i < ctx->n_sensors; i++) {
			rift_sensor_ctx *sensor_ctx = ctx->sensors[i];
			rift_sensor_update_exposure (sensor_ctx, &ctx->exposure_info);
		}
	}
}

void
rift_tracker_frame_start (rift_tracker_ctx *ctx, uint64_t local_ts, const char *source)
{
	int i;
	ohmd_lock_mutex (ctx->tracker_lock);
	for (i = 0; i < ctx->n_devices; i++) {
		rift_tracked_device_priv *dev = ctx->devices + i;

		ohmd_lock_mutex (dev->device_lock);
		rift_tracked_device_send_imu_debug(dev);

		ohmd_unlock_mutex (dev->device_lock);
	}
	ohmd_unlock_mutex (ctx->tracker_lock);
}

void
rift_tracker_frame_captured (rift_tracker_ctx *ctx, uint64_t local_ts, uint64_t frame_start_local_ts, rift_tracker_exposure_info *info, const char *source)
{
	int i;
	ohmd_lock_mutex (ctx->tracker_lock);
	for (i = 0; i < ctx->n_devices; i++) {
		rift_tracked_device_priv *dev = ctx->devices + i;

		ohmd_lock_mutex (dev->device_lock);

		/* This device might not have exposure info for this frame if it
		 * recently came online */
		if (i < info->n_devices) {
			rift_tracked_device_exposure_info *dev_info = info->devices + i;
			rift_tracked_device_frame_captured(dev, dev_info);
		}

		rift_tracked_device_send_imu_debug(dev);
		ohmd_unlock_mutex (dev->device_lock);
	}
	ohmd_unlock_mutex (ctx->tracker_lock);
}

void
rift_tracker_frame_release (rift_tracker_ctx *ctx, uint64_t local_ts, uint64_t frame_local_ts, rift_tracker_exposure_info *info, const char *source)
{
	int i;
	ohmd_lock_mutex (ctx->tracker_lock);
	for (i = 0; i < ctx->n_devices; i++) {
		rift_tracked_device_priv *dev = ctx->devices + i;

		ohmd_lock_mutex (dev->device_lock);

		/* This device might not have exposure info for this frame if it
		 * recently came online */
		if (i < info->n_devices) {
			rift_tracked_device_exposure_info *dev_info = info->devices + i;
			rift_tracked_device_frame_released(dev, dev_info);
		}

		rift_tracked_device_send_imu_debug(dev);
		ohmd_unlock_mutex (dev->device_lock);
	}
	ohmd_unlock_mutex (ctx->tracker_lock);
}

void
rift_tracker_free (rift_tracker_ctx *tracker_ctx)
{
	int i;

	if (!tracker_ctx)
		return;

	for (i = 0; i < tracker_ctx->n_sensors; i++) {
		rift_sensor_ctx *sensor_ctx = tracker_ctx->sensors[i];
		rift_sensor_free (sensor_ctx);
	}

	for (i = 0; i < RIFT_MAX_TRACKED_DEVICES; i++) {
		rift_tracked_device_priv *dev = tracker_ctx->devices + i;
		if (dev->base.led_search)
			led_search_model_free (dev->base.led_search);
		if (dev->debug_metadata != NULL)
			ohmd_pw_debug_stream_free (dev->debug_metadata);

		rift_kalman_6dof_clear(&dev->ukf_fusion);
		ohmd_destroy_mutex (dev->device_lock);
	}

	/* Stop USB event thread */
	tracker_ctx->usb_completed = true;
	ohmd_destroy_thread (tracker_ctx->usb_thread);

	if (tracker_ctx->usb_ctx)
		libusb_exit (tracker_ctx->usb_ctx);

	ohmd_destroy_mutex (tracker_ctx->tracker_lock);
	free (tracker_ctx);
}

void rift_tracked_device_imu_update(rift_tracked_device *dev_base, uint64_t local_ts, uint32_t device_ts, float dt, const vec3f* ang_vel, const vec3f* accel, const vec3f* mag_field)
{
	rift_tracked_device_priv *dev = (rift_tracked_device_priv *) (dev_base);
	rift_tracked_device_imu_observation *obs;

	ohmd_lock_mutex (dev->device_lock);
	ofusion_update(&dev->simple_fusion, dt, ang_vel, accel, mag_field);

	/* Handle device_ts wrap by extending to 64-bit and working in nanoseconds */
	if (dev->device_time_ns == 0) {
		dev->device_time_ns = device_ts * 1000;
	} else {
		uint64_t dt_ns = ((uint32_t)(device_ts - dev->last_device_ts)) * 1000;
		dev->device_time_ns += dt_ns;
	}
	dev->last_device_ts = device_ts;

	rift_kalman_6dof_imu_update (&dev->ukf_fusion, dev->device_time_ns, ang_vel, accel, mag_field);

	obs = dev->pending_imu_observations + dev->num_pending_imu_observations;
	obs->local_ts = local_ts;
	obs->device_ts = dev->device_time_ns;
	obs->dt = dt;
	obs->ang_vel = *ang_vel;
	obs->accel = *accel;
	obs->mag = *mag_field;
	obs->simple_orient = dev->simple_fusion.orient;

	dev->num_pending_imu_observations++;

	if (dev->num_pending_imu_observations == RIFT_MAX_PENDING_IMU_OBSERVATIONS) {
		/* No camera observations for a while - send our observations from here instead */
		rift_tracked_device_send_imu_debug(dev);
	}

	ohmd_unlock_mutex (dev->device_lock);
}

void rift_tracked_device_get_view_pose(rift_tracked_device *dev_base, posef *pose)
{
	rift_tracked_device_priv *dev = (rift_tracked_device_priv *) (dev_base);
	ohmd_lock_mutex (dev->device_lock);
	rift_kalman_6dof_get_pose_at(&dev->ukf_fusion, dev->device_time_ns, pose);
	ohmd_unlock_mutex (dev->device_lock);
}

static rift_tracker_pose_delay_slot *get_matching_delay_slot(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info);

void rift_tracked_device_model_pose_update(rift_tracked_device *dev_base, uint64_t local_ts, uint64_t frame_start_local_ts, rift_tracker_exposure_info *exposure_info, posef *pose, const char *source)
{
	rift_tracked_device_priv *dev = (rift_tracked_device_priv *) (dev_base);
	double time = (double)(exposure_info->local_ts) / 1000000000.0;
	rift_tracker_pose_delay_slot *slot = NULL;

	ohmd_lock_mutex (dev->device_lock);

	/* Undo any IMU to device conversion */
	oposef_apply_inverse(pose, &dev->fusion_to_model, pose);

	if (dev_base->id == 0) {
		/* Mirror the pose in XZ to go from device axes to view-plane */
		oposef_mirror_XZ(pose);
	}

	rift_tracked_device_send_imu_debug(dev);

	ofusion_tracker_update (&dev->simple_fusion, time, &pose->pos, &pose->orient);

	if (dev->index < exposure_info->n_devices) {
		/* This device existed when the exposure was taken and therefore has info */
		rift_tracked_device_exposure_info *dev_info = exposure_info->devices + dev->index;

		slot = get_matching_delay_slot(dev, dev_info);
		if (slot != NULL) {
			LOGD ("Got pose update for delay slot %d for dev %d, ts %llu (delay %f)", slot->slot_id, dev->base.id,
					(unsigned long long) frame_device_time_ns, (double) (dev->device_time_ns - frame_device_time_ns) / 1000000000.0 );
			rift_kalman_6dof_position_update(&dev->ukf_fusion, dev->device_time_ns, pose, slot->slot_id);
		}
	}

	ohmd_unlock_mutex (dev->device_lock);
}

void rift_tracked_device_get_model_pose(rift_tracked_device *dev_base, double ts, posef *pose, float *gravity_error_rad)
{
	rift_tracked_device_priv *dev = (rift_tracked_device_priv *) (dev_base);
	posef tmp;
	ohmd_lock_mutex (dev->device_lock);

	rift_kalman_6dof_get_pose_at(&dev->ukf_fusion, dev->device_time_ns, &tmp);

	if (dev_base->id == 0) {
		/* Mirror the pose in XZ to go from view-plane to device axes for the HMD */
		oposef_mirror_XZ(&tmp);
	}

	/* Apply any needed global pose change */
	oposef_apply(&tmp, &dev->fusion_to_model, pose);

	/* FIXME: Return a real value based on orientation covariance, when the filtering can supply that.
	 * For now, check that there was a recent gravity update and it was small */
	if (gravity_error_rad) {
		double time_since_gravity = (ts - dev->simple_fusion.last_gravity_vector_time);
		if (time_since_gravity > -0.5 && time_since_gravity < 0.5) {
			*gravity_error_rad = dev->simple_fusion.grav_error_angle;
		}
		else {
			*gravity_error_rad = M_PI;
		}
	}

	ohmd_unlock_mutex (dev->device_lock);
}

/* Called with the device lock held */
static void
rift_tracked_device_send_imu_debug(rift_tracked_device_priv *dev)
{
	int i;

	if (dev->num_pending_imu_observations == 0)
		return;

	if (dev->debug_metadata && ohmd_pw_debug_stream_connected(dev->debug_metadata)) {
		char debug_str[1024];

		for (i = 0; i < dev->num_pending_imu_observations; i++) {
			rift_tracked_device_imu_observation *obs = dev->pending_imu_observations + i;

			snprintf (debug_str, 1024, ",\n{ \"type\": \"imu\", \"local-ts\": %llu, "
				 "\"device-ts\": %llu, \"dt\": %f, "
				 "\"ang_vel\": [ %f, %f, %f ], \"accel\": [ %f, %f, %f ], "
				 "\"mag\": [ %f, %f, %f ], "
				 "\"simple-orient\" : [ %f, %f, %f, %f ] }",
				(unsigned long long) obs->local_ts,
				(unsigned long long) obs->device_ts,
				obs->dt,
				obs->ang_vel.x, obs->ang_vel.y, obs->ang_vel.z,
				obs->accel.x, obs->accel.y, obs->accel.z,
				obs->mag.x, obs->mag.y, obs->mag.z,
				obs->simple_orient.x, obs->simple_orient.y, obs->simple_orient.z, obs->simple_orient.w);

			debug_str[1023] = '\0';

			ohmd_pw_debug_stream_push (dev->debug_metadata, obs->local_ts, debug_str);
		}
	}

	dev->num_pending_imu_observations = 0;
}

static rift_tracker_pose_delay_slot *
find_free_delay_slot(rift_tracked_device_priv *dev)
{
	/* Pose observation delay slots */
	for (int i = 0; i < NUM_POSE_DELAY_SLOTS; i++) {
		int slot_no = dev->delay_slot_index;
		rift_tracker_pose_delay_slot *slot = dev->delay_slots + slot_no;

		/* Cycle through the free delay slots */
		dev->delay_slot_index = (slot_no+1) % NUM_POSE_DELAY_SLOTS;

		if (slot->use_count == 0)
			return slot;
	}

	/* Failed to find a free slot */
	return NULL;
}

static rift_tracker_pose_delay_slot *
get_matching_delay_slot(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info)
{
	rift_tracker_pose_delay_slot *slot = NULL;
	int slot_no = dev_info->fusion_slot;

	if (slot_no != -1) {
		assert (slot_no < NUM_POSE_DELAY_SLOTS);
		slot = dev->delay_slots + slot_no;
	}

	if (slot && slot->valid && slot->device_time_ns == dev_info->device_time_ns)
		return slot;

	return NULL;
}

/* Called with the device lock held */
static void
rift_tracked_device_update_exposure(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info) {
	rift_tracker_pose_delay_slot *slot = find_free_delay_slot(dev);

	dev_info->device_time_ns = dev->device_time_ns;

	if (slot) {
		slot->device_time_ns = dev_info->device_time_ns;
		slot->valid = true;
		dev_info->fusion_slot = slot->slot_id;

		LOGD ("Assigning free delay slot %d for dev %d, ts %llu", slot->slot_id, dev->base.id, (unsigned long long) dev->device_time_ns);

		/* Tell the kalman filter to prepare the delay slot */
		rift_kalman_6dof_prepare_delay_slot(&dev->ukf_fusion, dev_info->device_time_ns, slot->slot_id);
	}
	else {
		LOGW ("No free delay slot for dev %d, ts %llu", dev->base.id, (unsigned long long) dev->device_time_ns);
		dev_info->fusion_slot = -1;
	}
}

static void
rift_tracked_device_frame_captured(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info)
{
	rift_tracker_pose_delay_slot *slot = get_matching_delay_slot(dev, dev_info);

	/* There is a delay slot for this frame, claim it */
	if (slot) {
		slot->use_count++;
		dev_info->fusion_slot = slot->slot_id;
	}
	else {
		/* The slot was not allocated (we missed the exposure event), or it
		 * was overridden by a later exposure because there's not enough slots */
		if (dev_info->fusion_slot != -1) {
			LOGW ("Lost delay slot for dev %d, ts %llu", dev->base.id, (unsigned long long) dev_info->device_time_ns);
		}
		dev_info->fusion_slot = -1;
	}
}

static void
rift_tracked_device_frame_released(rift_tracked_device_priv *dev, rift_tracked_device_exposure_info *dev_info)
{
	rift_tracker_pose_delay_slot *slot = get_matching_delay_slot(dev, dev_info);

	/* There is a delay slot for this frame, release it */
	if (slot) {
		if (slot->use_count > 0)
			slot->use_count--;

		if (slot->use_count == 0) {
			slot->valid = false;

			/* Tell the kalman filter the slot is invalid */
			rift_kalman_6dof_release_delay_slot(&dev->ukf_fusion, slot->slot_id);
		}
	}
}