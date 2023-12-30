/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

#include <pulsecore/source.h>
#include <pulsecore/namereg.h>

PA_MODULE_AUTHOR("AGL IC EG");
PA_MODULE_DESCRIPTION(_("Volume attenuator"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "sink_name=<name of sink> "
        "target_name=<name of source> "
        );

#define DEFAULT_SINK_NAME "attenuator"
#define BLOCK_USEC (2 * PA_USEC_PER_SEC)
#define BLOCK_USEC_NOREWINDS (50 * PA_USEC_PER_MSEC)

#define ATTENUATE_MESSAGE_MUTE		(1)
#define ATTENUATE_MESSAGE_UNMUTE	(0)

typedef struct s_attenuate_msg attenuate_msg_t;

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
	
	attenuate_msg_t *msg;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_usec_t block_usec;
    pa_usec_t timestamp;

    pa_idxset *formats;

    bool norewinds;
	
	pa_source *source;
};

struct s_attenuate_msg {
    pa_msgobject parent;
    struct userdata *userdata;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "target_name",
    NULL
};

static int sink_process_msg(
        pa_msgobject *o,
        int code,
        void *data,
        int64_t offset,
        pa_memchunk *chunk) {

    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t now;

            now = pa_rtclock_now();
            *((int64_t*) data) = (int64_t)u->timestamp - (int64_t)now;

            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    if (s->thread_info.state == PA_SINK_SUSPENDED || s->thread_info.state == PA_SINK_INIT) {
        if (PA_SINK_IS_OPENED(new_state))
            u->timestamp = pa_rtclock_now();
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = pa_sink_get_requested_latency_within_thread(s);

    if (u->block_usec == (pa_usec_t) -1)
        u->block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(u->block_usec, &s->sample_spec);

        pa_sink_set_max_rewind_within_thread(s, nbytes);

    pa_sink_set_max_request_within_thread(s, nbytes);
}

static void sink_reconfigure_cb(pa_sink *s, pa_sample_spec *spec, bool passthrough) {
    /* We don't need to do anything */
    s->sample_spec = *spec;
}

static bool sink_set_formats_cb(pa_sink *s, pa_idxset *formats) {
    struct userdata *u = s->userdata;

    pa_assert(u);

    pa_idxset_free(u->formats, (pa_free_cb_t) pa_format_info_free);
    u->formats = pa_idxset_copy(formats, (pa_copy_func_t) pa_format_info_copy);

    return true;
}

static pa_idxset* sink_get_formats_cb(pa_sink *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);

    return pa_idxset_copy(u->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void process_rewind(struct userdata *u, pa_usec_t now) {
    size_t rewind_nbytes, in_buffer;
    pa_usec_t delay;

    pa_assert(u);

    rewind_nbytes = u->sink->thread_info.rewind_nbytes;

    if (!PA_SINK_IS_OPENED(u->sink->thread_info.state) || rewind_nbytes <= 0)
        goto do_nothing;

    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    if (u->timestamp <= now)
        goto do_nothing;

    delay = u->timestamp - now;
    in_buffer = pa_usec_to_bytes(delay, &u->sink->sample_spec);

    if (in_buffer <= 0)
        goto do_nothing;

    if (rewind_nbytes > in_buffer)
        rewind_nbytes = in_buffer;

    pa_sink_process_rewind(u->sink, rewind_nbytes);
    u->timestamp -= pa_bytes_to_usec(rewind_nbytes, &u->sink->sample_spec);

    pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
    return;

do_nothing:

    pa_sink_process_rewind(u->sink, 0);
}

static int64_t get_current_time_ms(void)
{
	int64_t ms = -1;
	struct timespec t = {0,0};
	int ret = -1;

	ret = clock_gettime(CLOCK_MONOTONIC, &t);
	if (ret == 0) {
		ms = ((int64_t)t.tv_sec * 1000) + ((int64_t)t.tv_nsec / 1000 / 1000);
	}

	return ms;
}

static void set_mute(struct userdata *u, int mute)
{
	if (mute == 1) {
		pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->msg), ATTENUATE_MESSAGE_MUTE, NULL, 0, NULL, NULL);
	} else {
		pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->msg), ATTENUATE_MESSAGE_UNMUTE, NULL, 0, NULL, NULL);
	}
}

static int g_silent_count = 2000;
static int64_t g_last_time = 0;
static int g_attenuate_on = 0;

static void test_volume(struct userdata *u, pa_memchunk *pchunk)
{
	size_t index = 0;
	size_t length = 0;
	int8_t *pb = NULL;
	int16_t *psw = NULL;
	int16_t val16 = 0;
	int64_t val64 = 0;
	int64_t current_time = 0;

    index = pchunk->index;
    length = pchunk->length;
	pb = (int8_t*)pa_memblock_acquire(pchunk->memblock);

	psw = (int16_t*)(pb + index);
	psw = (int16_t*)(pb);

	for (int i=0; i < (length/2); i++) {
		val16 = psw[i];
		if (val16 > 0) {
			val64 = val64 + (int64_t)val16;
		} else {
			val64 = val64 - (int64_t)val16;
		}
	}

	val64 = val64 / (length/2);

	current_time = get_current_time_ms();
	if (val64 > 1024) {
		if (g_attenuate_on == 0) {
			g_attenuate_on = 1;
			//fprintf(stderr, "Attenuate ON\n");
			if (u->source != NULL) {
				set_mute(u, 1);
			}
		}
		g_silent_count = 0;
	} else {
		if (g_silent_count >= 500) {
			if (g_attenuate_on == 1) {
				//fprintf(stderr, "Attenuate OFF\n");
				if (u->source != NULL) {
					set_mute(u, 0);
				}
				g_attenuate_on = 0;
			}
		}
		g_silent_count = g_silent_count + (current_time - g_last_time);
	}
	g_last_time = current_time;

    pa_memblock_release(pchunk->memblock);

	return;
}

static void process_render(struct userdata *u, pa_usec_t now) {
    size_t ate = 0;

    pa_assert(u);

    /* This is the configured latency. Sink inputs connected to us
    might not have a single frame more than the maxrequest value
    queued. Hence: at maximum read this many bytes from the sink
    inputs. */

    /* Fill the buffer up the latency size */
    while (u->timestamp < now + u->block_usec) {
        pa_memchunk chunk;
        size_t request_size;

        request_size = pa_usec_to_bytes(now + u->block_usec - u->timestamp, &u->sink->sample_spec);
        request_size = PA_MIN(request_size, u->sink->thread_info.max_request);
        pa_sink_render(u->sink, request_size, &chunk);

		test_volume(u, &chunk);

        pa_memblock_unref(chunk.memblock);

/*         pa_log_debug("Ate %lu bytes.", (unsigned long) chunk.length); */
        u->timestamp += pa_bytes_to_usec(chunk.length, &u->sink->sample_spec);

        ate += chunk.length;

        if (ate >= u->sink->thread_info.max_request)
            break;
    }

/*     pa_log_debug("Ate in sum %lu bytes (of %lu)", (unsigned long) ate, (unsigned long) nbytes); */
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->realtime_scheduling)
        pa_thread_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for (;;) {
        pa_usec_t now = 0;
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            now = pa_rtclock_now();

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            process_rewind(u, now);

        /* Render some data and drop it immediately */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->timestamp <= now)
                process_render(u, now);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

PA_DEFINE_PRIVATE_CLASS(attenuate_msg_t, pa_msgobject);
#define ATTENUATE_MSG(o) (attenuate_msg_t_cast(o))
/* Called from main context */
static int attenuate_process_msg_cb(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    struct s_attenuate_msg *msg;
    struct userdata *u;

    pa_assert(o);
    pa_assert_ctl_context();

    msg = ATTENUATE_MSG(o);
	pa_assert_se(u = msg->userdata);

	if (code == ATTENUATE_MESSAGE_MUTE) {
		fprintf(stderr, "Attenuate MUTE GET\n");
		if (u->source != NULL) {
			pa_cvolume cvolume;
			pa_cvolume_set(&cvolume, 1, PA_VOLUME_MUTED);
			pa_source_set_soft_volume(u->source, &cvolume);
		}
	} else {
		fprintf(stderr, "Attenuate UNMUTE GET\n");
		if (u->source != NULL) {
			pa_cvolume cvolume;
			pa_cvolume_set(&cvolume, 1, PA_VOLUME_NORM);
			pa_source_set_soft_volume(u->source, &cvolume);
		}
	}

    return 0;
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    pa_sink_new_data data;
    pa_format_info *format;
    size_t nbytes;
	const char *target_name = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();
    u->block_usec = BLOCK_USEC;

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        goto fail;
    }

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Volume attenuator"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

    u->formats = pa_idxset_new(NULL, NULL);
    format = pa_format_info_new();
    format->encoding = PA_ENCODING_PCM;
    pa_idxset_put(u->formats, format, NULL);

	target_name = pa_modargs_get_value(ma, "target_name", NULL);
	if (target_name == NULL)
	{
		fprintf(stderr, "Volume attenuator is not set.\n");
        pa_log("Volume attenuator is not set.");
        goto fail;
	}

	u->source = pa_namereg_get(m->core, target_name, PA_NAMEREG_SOURCE);
	if (u->source == NULL)
	{
		fprintf(stderr, "Can't get target %s.\n", target_name);
        pa_log("Can't get targe.");
        goto fail;
	}

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY | PA_SINK_SET_FORMATS);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->reconfigure = sink_reconfigure_cb;
    u->sink->get_formats = sink_get_formats_cb;
    u->sink->set_formats = sink_set_formats_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);

    pa_sink_set_max_rewind(u->sink, nbytes);

    pa_sink_set_max_request(u->sink, nbytes);

	/* Setup message handler for main thread */
    u->msg = pa_msgobject_new(attenuate_msg_t);
    u->msg->parent.process_msg = attenuate_process_msg_cb;
    u->msg->userdata = u;
	
	
    if (!(u->thread = pa_thread_new("attenuator", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_set_latency_range(u->sink, 0, u->block_usec);

    pa_sink_put(u->sink);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->formats)
        pa_idxset_free(u->formats, (pa_free_cb_t) pa_format_info_free);

    pa_xfree(u);
}
