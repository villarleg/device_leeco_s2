/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>

#define LOG_TAG "QTI PowerHAL"
#include <utils/Log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

#define BUS_SPEED_PATH "/sys/class/devfreq/gpubw/min_freq"
#define GPU_MAX_FREQ_PATH "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq"
#define GPU_MIN_FREQ_PATH "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq"

static int saved_interactive_mode = -1;
static int display_hint_sent;
static int video_encode_hint_sent;
static int sustained_performance_mode = 0;
static int vr_mode = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void process_video_encode_hint(void *metadata);

int  power_hint_override(struct power_module *module, power_hint_t hint,
        void *data)
{
    static int handle_hotplug = 0;
    int resources_hotplug[] = {0x3DFF};

    switch(hint) {
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE:
        {
            process_video_encode_hint(data);
            return HINT_HANDLED;
        }
        case POWER_HINT_INTERACTION:
        {
            int ret;

            pthread_mutex_lock(&lock);
            if (sustained_performance_mode || vr_mode)
                ret = HINT_HANDLED;
            else
                ret = HINT_NONE;
            pthread_mutex_unlock(&lock);
            return ret;
        }
        case POWER_HINT_SUSTAINED_PERFORMANCE:
        {
            pthread_mutex_lock(&lock);
            if (data && sustained_performance_mode == 0) {
                sysfs_write(GPU_MAX_FREQ_PATH, "432000000");
                if (vr_mode == 0) {
                    handle_hotplug = interaction_with_handle(handle_hotplug, 0,
                                        sizeof(resources_hotplug)/sizeof(resources_hotplug[0]),
                                        resources_hotplug);
                }
                sustained_performance_mode = 1;
            } else if (!data && sustained_performance_mode == 1) {
                sysfs_write(GPU_MAX_FREQ_PATH, "600000000");
                if (vr_mode == 0) {
                    release_request(handle_hotplug);
                }
                sustained_performance_mode = 0;
           }
           pthread_mutex_unlock(&lock);
           return HINT_HANDLED;
        }
        break;
        case POWER_HINT_VR_MODE:
        {
            pthread_mutex_lock(&lock);
            if (data && vr_mode == 0) {
                sysfs_write(GPU_MIN_FREQ_PATH, "432000000");
                sysfs_write(BUS_SPEED_PATH, "2929");
                if (sustained_performance_mode == 0) {
                    handle_hotplug = interaction_with_handle(handle_hotplug, 0,
                                        sizeof(resources_hotplug)/sizeof(resources_hotplug[0]),
                                        resources_hotplug);
                }
                vr_mode = 1;
            } else if (vr_mode == 1){
                sysfs_write(GPU_MIN_FREQ_PATH, "266666667");
                sysfs_write(BUS_SPEED_PATH, "0");
                if (sustained_performance_mode == 0) {
                    release_request(handle_hotplug);
                }
                vr_mode = 0;
            }
            pthread_mutex_unlock(&lock);
            return HINT_HANDLED;
        }
    }
    return HINT_NONE;
}

int  set_interactive_override(struct power_module *module, int on)
{
    char governor[80];

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_HANDLED;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
             if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
               int resource_values[] = {INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                                        INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                                        INT_OP_NOTIFY_ON_MIGRATE, 0x00};

               if (!display_hint_sent) {
                   perform_hint_action(DISPLAY_STATE_HINT_ID,
                   resource_values, sizeof(resource_values)/sizeof(resource_values[0]));
                  display_hint_sent = 1;
                }
             } /* Perf time rate set for CORE0,CORE4 8952 target*/

    } else {
        /* Display on. */
          if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {

             undo_hint_action(DISPLAY_STATE_HINT_ID);
             display_hint_sent = 0;
          }
   }
    saved_interactive_mode = !!on;
    return HINT_HANDLED;
}

/* Video Encode Hint */
static void process_video_encode_hint(void *metadata)
{
    char governor[80];
    struct video_encode_metadata_t video_encode_metadata;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor_check_cores(governor,
        sizeof(governor),CPU0) == -1) {
            if (get_scaling_governor_check_cores(governor,
                sizeof(governor),CPU1) == -1) {
                    if (get_scaling_governor_check_cores(governor,
                        sizeof(governor),CPU2) == -1) {
                            if (get_scaling_governor_check_cores(governor,
                                sizeof(governor),CPU3) == -1) {
                                    ALOGE("Can't obtain scaling governor.");
                                    return;
                            }
                    }
            }
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (metadata) {
        if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
            ALOGE("Error occurred while parsing metadata.");
            return;
        }
    } else {
        return;
    }

    if (video_encode_metadata.state == 1) {
        if ((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            /* Sched_load and migration_notif*/
            int resource_values[] = {INT_OP_CLUSTER0_USE_SCHED_LOAD,
                                     0x1,
                                     INT_OP_CLUSTER1_USE_SCHED_LOAD,
                                     0x1,
                                     INT_OP_CLUSTER0_USE_MIGRATION_NOTIF,
                                     0x1,
                                     INT_OP_CLUSTER1_USE_MIGRATION_NOTIF,
                                     0x1,
                                     INT_OP_CLUSTER0_TIMER_RATE,
                                     BIG_LITTLE_TR_MS_40,
                                     INT_OP_CLUSTER1_TIMER_RATE,
                                     BIG_LITTLE_TR_MS_40
                                     };
            if (!video_encode_hint_sent) {
                perform_hint_action(video_encode_metadata.hint_id,
                resource_values,
                sizeof(resource_values)/sizeof(resource_values[0]));
                video_encode_hint_sent = 1;
            }
        }
    } else if (video_encode_metadata.state == 0) {
        if ((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            undo_hint_action(video_encode_metadata.hint_id);
            video_encode_hint_sent = 0;
            return ;
        }
    }
    return;
}

#define DT2W_PATH "/sys/android_touch/doubletap2wake"
#define S2W_PATH "/sys/android_touch/sweep2wake"
#define VIB_PATH "/sys/android_touch/vib_strength"

void set_feature(struct power_module *module, feature_t feature, int state)
{
    if (feature == POWER_FEATURE_DOUBLE_TAP_TO_WAKE) {
        int mode, fd;
        char buffer[16];

        mode = property_get_int32("persist.wake_gesture.mode", 0);
        if (mode < 0)
        	mode = 0;

        fd = open(DT2W_PATH, O_WRONLY);
        if (fd >= 0) {
        	snprintf(buffer, 16, "%d", state ? !mode : 0);
            write(fd, buffer, strlen(buffer) + 1);
            close(fd);
        }

        fd = open(S2W_PATH, O_WRONLY);
        if (fd >= 0) {
        	snprintf(buffer, 16, "%d", state ? mode : 0);
            write(fd, buffer, strlen(buffer) + 1);
            close(fd);
        }

        mode = property_get_int32("persist.wake_gesture.vib_strength", -1);
        if (mode >= 0) {
        	fd = open(VIB_PATH, O_WRONLY);
            if (fd >= 0) {
        	    snprintf(buffer, 16, "%d", mode);
                write(fd, buffer, strlen(buffer) + 1);
                close(fd);
            }
        }
    }
}
