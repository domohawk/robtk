/* x42 jack wrapper
 *
 * Copyright (C) 2012, 2013 Robin Gareus
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef UPDATE_FREQ_RATIO
#define UPDATE_FREQ_RATIO 10 // MAX # of audio-cycles per GUI-refresh
#endif

#ifndef UI_UPDATE_FPS
#define UI_UPDATE_FPS 15
#endif

///////////////////////////////////////////////////////////////////////////////

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>
#include <sys/mman.h>
#include <assert.h>

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"

#include "./gl/xternalui.h"

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

#define LV2_EXTERNAL_UI_RUN(ptr) (ptr)->run(ptr)
#define LV2_EXTERNAL_UI_SHOW(ptr) (ptr)->show(ptr)
#define LV2_EXTERNAL_UI_HIDE(ptr) (ptr)->hide(ptr)

#define nan NAN

const LV2_Descriptor* plugin_dsp;
const LV2UI_Descriptor *plugin_gui;

LV2_Handle plugin_instance = NULL;
LV2UI_Handle gui_instance = NULL;

float  *plugin_ports_pre  = NULL;
float  *plugin_ports_post = NULL;

LV2_Atom_Sequence *atom_in = NULL;
LV2_Atom_Sequence *atom_out = NULL;

static jack_port_t **input_port = NULL;
static jack_port_t **output_port = NULL;

static jack_port_t *midi_in = NULL;
static jack_port_t *midi_out = NULL;

static jack_client_t *j_client = NULL;
static uint32_t j_samplerate = 48000;

struct transport_position {
	jack_nframes_t position;
	float          bpm;
	bool           rolling;
} j_transport = {0, 0, false};

static jack_ringbuffer_t *rb_ctrl_to_ui = NULL;
static jack_ringbuffer_t *rb_ctrl_from_ui = NULL;
static jack_ringbuffer_t *rb_atom_to_ui = NULL;
static jack_ringbuffer_t *rb_atom_from_ui = NULL;

static pthread_mutex_t gui_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

extern const LV2_Descriptor* lv2_descriptor(uint32_t index);
extern const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index);

uint32_t uri_midi_MidiEvent = 0;
uint32_t uri_atom_Sequence = 0;
uint32_t uri_atom_EventTransfer = 0;

uint32_t uri_time_Position = 0;
uint32_t uri_time_frame    = 0;
uint32_t uri_time_speed    = 0;
uint32_t uri_time_bar     = 0;
uint32_t uri_time_barBeat = 0;
uint32_t uri_time_beatUnit = 0;
uint32_t uri_time_beatsPerBar = 0;
uint32_t uri_time_beatsPerMinute = 0;

char **urimap = NULL;
uint32_t urimap_len = 0;

enum PortType {
	CONTROL_IN = 0,
	CONTROL_OUT,
	AUDIO_IN,
	AUDIO_OUT,
	MIDI_IN,
	MIDI_OUT,
	ATOM_IN,
	ATOM_OUT
};

struct LV2Port {
	const char *name;
	enum PortType porttype;
	float val_default;
};

/* a simple state machine for this client */
static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;

struct lv2_external_ui_host extui_host;
struct lv2_external_ui *extui = NULL;

LV2UI_Controller controller = NULL;

LV2_Atom_Forge lv2_forge;
uint32_t *portmap_a_in;
uint32_t *portmap_a_out;
uint32_t *portmap_rctl;
int      *portmap_ctrl;
uint32_t  portmap_atom_to_ui = -1;
uint32_t  portmap_atom_from_ui = -1;

static uint32_t uri_to_id(LV2_URI_Map_Callback_Data callback_data, const char* uri);

///////////////////////////
// GET INFO FROM LV2 TTL //
//     see lv2ttl2c      //
///////////////////////////
#include JACK_DESCRIPT ////
///////////////////////////

/******************************************************************************
 * JACK
 */

int process (jack_nframes_t nframes, void *arg) {
	while (jack_ringbuffer_read_space(rb_ctrl_from_ui) >= sizeof(uint32_t) + sizeof(float)) {
		uint32_t idx;
		jack_ringbuffer_read(rb_ctrl_from_ui, (char*) &idx, sizeof(uint32_t));
		jack_ringbuffer_read(rb_ctrl_from_ui, (char*) &(plugin_ports_pre[idx]), sizeof(float));
	}

	/* Get Jack transport position */
	jack_position_t pos;
	const bool rolling = (jack_transport_query(j_client, &pos) == JackTransportRolling);
	const bool transport_changed = (rolling != j_transport.rolling
			|| pos.frame != j_transport.position
			|| ((pos.valid & JackPositionBBT) && (pos.beats_per_minute != j_transport.bpm)));

	/* atom buffers */
	if (nports_atom_in > 0 || nports_midi_in > 0) {
		/* start Atom sequence */
		atom_in->atom.type = uri_atom_Sequence;
		atom_in->atom.size = 8;
		LV2_Atom_Sequence_Body *body = &atom_in->body;
		body->unit = 0; // URID of unit of event time stamp LV2_ATOM__timeUnit ??
		body->pad  = 0; // unused
		uint8_t * seq = (uint8_t*) (body + 1);

		if (transport_changed && send_time_info) {
			uint8_t   pos_buf[256];
			LV2_Atom* lv2_pos = (LV2_Atom*)pos_buf;

			lv2_atom_forge_set_buffer(&lv2_forge, pos_buf, sizeof(pos_buf));
			LV2_Atom_Forge* forge = &lv2_forge;
			LV2_Atom_Forge_Frame frame;
			lv2_atom_forge_blank(&lv2_forge, &frame, 1, uri_time_Position);
			lv2_atom_forge_property_head(forge, uri_time_frame, 0);
			lv2_atom_forge_long(forge, pos.frame);
			lv2_atom_forge_property_head(forge, uri_time_speed, 0);
			lv2_atom_forge_float(forge, rolling ? 1.0 : 0.0);
			if (pos.valid & JackPositionBBT) {
				lv2_atom_forge_property_head(forge, uri_time_barBeat, 0);
				lv2_atom_forge_float(
						forge, pos.beat - 1 + (pos.tick / pos.ticks_per_beat));
				lv2_atom_forge_property_head(forge, uri_time_bar, 0);
				lv2_atom_forge_long(forge, pos.bar - 1);
				lv2_atom_forge_property_head(forge, uri_time_beatUnit, 0);
				lv2_atom_forge_int(forge, pos.beat_type);
				lv2_atom_forge_property_head(forge, uri_time_beatsPerBar, 0);
				lv2_atom_forge_float(forge, pos.beats_per_bar);
				lv2_atom_forge_property_head(forge, uri_time_beatsPerMinute, 0);
				lv2_atom_forge_float(forge, pos.beats_per_minute);
			}

			uint32_t size = lv2_pos->size;
			uint32_t padded_size = ((sizeof(LV2_Atom_Event) + size) +  7) & (~7);

			if (min_atom_bufsiz > padded_size) {
				printf("send time..\n");
				LV2_Atom_Event *aev = (LV2_Atom_Event *)seq;
				aev->time.frames = 0;
				aev->body.size   = size;
				aev->body.type   = lv2_pos->type;
				memcpy(LV2_ATOM_BODY(&aev->body), LV2_ATOM_BODY(lv2_pos), size);
				atom_in->atom.size += padded_size;
				seq +=  padded_size;
			}
		}
		// TODO only if UI..?
		while (jack_ringbuffer_read_space(rb_atom_from_ui) > sizeof(LV2_Atom)) {
			LV2_Atom a;
			jack_ringbuffer_read(rb_atom_from_ui, (char *) &a, sizeof(LV2_Atom));
			uint32_t padded_size = atom_in->atom.size + a.size + sizeof(int64_t);
			if (min_atom_bufsiz > padded_size) {
				memset(seq, 0, sizeof(int64_t)); // LV2_Atom_Event->time
				seq += sizeof(int64_t);
				jack_ringbuffer_read(rb_atom_from_ui, (char *) seq, a.size);
				seq += a.size;
				atom_in->atom.size += a.size + sizeof(int64_t);
			}
		}
		if (nports_midi_in > 0) {
			/* inject midi events */
			void* buf = jack_port_get_buffer(midi_in, nframes);
			for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
				jack_midi_event_t ev;
				jack_midi_event_get(&ev, buf, i);

				uint32_t size = ev.size;
				uint32_t padded_size = ((sizeof(LV2_Atom_Event) + size) +  7) & (~7);

				if (min_atom_bufsiz > padded_size) {
					LV2_Atom_Event *aev = (LV2_Atom_Event *)seq;
					aev->time.frames = ev.time;
					aev->body.size  = size;
					aev->body.type  = uri_midi_MidiEvent;
					memcpy(LV2_ATOM_BODY(&aev->body), ev.buffer, size);
					atom_in->atom.size += padded_size;
					seq += padded_size;
				}
			}
		}
	}

	if (nports_atom_out > 0 || nports_midi_out > 0) {
		atom_out->atom.type = 0;
		atom_out->atom.size = min_atom_bufsiz;
	}

	/* [re] connect jack audio buffers */
  for (uint32_t i=0; i < nports_audio_in; i++) {
		plugin_dsp->connect_port(plugin_instance, portmap_a_in[i], jack_port_get_buffer (input_port[i], nframes));
  }
  for (uint32_t i=0 ; i < nports_audio_out; i++) {
		plugin_dsp->connect_port(plugin_instance, portmap_a_out[i], jack_port_get_buffer (output_port[i], nframes));
  }

	/* make a backup copy, to see what was changed */
	memcpy(plugin_ports_post, plugin_ports_pre, nports_ctrl * sizeof(float));

	/* expected transport state in next cycle */
	j_transport.position = rolling ? pos.frame + nframes : pos.frame;
	j_transport.bpm      = pos.beats_per_minute;
	j_transport.rolling  = rolling;

	/* run the plugin */
	plugin_dsp->run(plugin_instance, nframes);

	/* create port-events for change values */
	// TODO only if UI..?
	for (uint32_t p = 0; p < nports_ctrl; p++) {
		if (ports[portmap_rctl[p]].porttype != CONTROL_OUT) continue;

		if (plugin_ports_pre[p] != plugin_ports_post[p]) {
			if (jack_ringbuffer_write_space(rb_ctrl_to_ui) >= sizeof(uint32_t) + sizeof(float)) {
				jack_ringbuffer_write(rb_ctrl_to_ui, (char *) &portmap_rctl[p], sizeof(uint32_t));
				jack_ringbuffer_write(rb_ctrl_to_ui, (char *) &plugin_ports_pre[p], sizeof(float));
			}
		}
	}

	if (nports_midi_out > 0) {
		void* buf = jack_port_get_buffer(midi_out, nframes);
		jack_midi_clear_buffer(buf);
	}

	/* Atom sequence port-events */
	if (nports_atom_out + nports_midi_out > 0 && atom_out->atom.size > sizeof(LV2_Atom)) {
		// TODO only if UI..?
		if (jack_ringbuffer_write_space(rb_atom_to_ui) >= atom_out->atom.size + 2 * sizeof(LV2_Atom)) {
			LV2_Atom a = {atom_out->atom.size + (uint32_t) sizeof(LV2_Atom), 0};
			jack_ringbuffer_write(rb_atom_to_ui, (char *) &a, sizeof(LV2_Atom));
			jack_ringbuffer_write(rb_atom_to_ui, (char *) atom_out, a.size);
		}

		if (nports_midi_out) {
			void* buf = jack_port_get_buffer(midi_out, nframes);
			LV2_Atom_Event const* ev = (LV2_Atom_Event const*)((&(atom_out)->body) + 1); // lv2_atom_sequence_begin
			while((const uint8_t*)ev < ((const uint8_t*) &(atom_out)->body + (atom_out)->atom.size)) {
				if (ev->body.type == uri_midi_MidiEvent) {
					jack_midi_event_write(buf, ev->time.frames, (const uint8_t*)(ev+1), ev->body.size);
				}
				ev = (LV2_Atom_Event const*) /* lv2_atom_sequence_next() */
					((const uint8_t*)ev + sizeof(LV2_Atom_Event) + ((ev->body.size + 7) & ~7));
			}
		}
	}

	/* wake up UI */
	if (jack_ringbuffer_read_space(rb_ctrl_to_ui) > sizeof(uint32_t) + sizeof(float)
			|| jack_ringbuffer_read_space(rb_atom_to_ui) > sizeof(LV2_Atom)
			) {
		if (pthread_mutex_trylock (&gui_thread_lock) == 0) {
			pthread_cond_signal (&data_ready);
			pthread_mutex_unlock (&gui_thread_lock);
		}
	}
  return 0;
}


void jack_shutdown (void *arg) {
  fprintf(stderr,"recv. shutdown request from jackd.\n");
  client_state=Exit;
  pthread_cond_signal (&data_ready);
}

static int init_jack(const char *client_name) {
  jack_status_t status;
  j_client = jack_client_open (client_name, JackNullOption, &status);
  if (j_client == NULL) {
    fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    return (-1);
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(j_client);
    fprintf (stderr, "jack-client name: `%s'\n", client_name);
  }

  jack_set_process_callback (j_client, process, 0);

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  j_samplerate=jack_get_sample_rate (j_client);
  return (0);
}

static int jack_portsetup(void) {
  /* Allocate data structures that depend on the number of ports. */
  input_port = (jack_port_t **) malloc (sizeof (jack_port_t *) * nports_audio_in);

  for (uint32_t i = 0; i < nports_audio_in; i++) {
    if ((input_port[i] = jack_port_register (j_client,
						ports[portmap_a_in[i]].name,
						JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
      fprintf (stderr, "cannot register input port \"%s\"!\n", ports[portmap_a_in[i]].name);
      return (-1);
    }
  }

  output_port = (jack_port_t **) malloc (sizeof (jack_port_t *) * nports_audio_out);

  for (uint32_t i = 0; i < nports_audio_out; i++) {
    if ((output_port[i] = jack_port_register (j_client,
						ports[portmap_a_out[i]].name,
						JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0)) == 0) {
      fprintf (stderr, "cannot register output port \"%s\"!\n", ports[portmap_a_out[i]].name);
      return (-1);
    }
  }

	if (nports_midi_in){
		if ((midi_in = jack_port_register (j_client,
						ports[portmap_atom_from_ui].name,
						JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)) == 0) {
			fprintf (stderr, "cannot register midi input port \"%s\"!\n", ports[portmap_atom_from_ui].name);
			return (-1);
		}
	}

	if (nports_midi_out){
		if ((midi_out = jack_port_register (j_client,
						ports[portmap_atom_to_ui].name,
						JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
			fprintf (stderr, "cannot register midi ouput port \"%s\"!\n", ports[portmap_atom_to_ui].name);
			return (-1);
		}
	}
	return (0);
}

/******************************************************************************
 * LV2
 */

static uint32_t uri_to_id(LV2_URI_Map_Callback_Data callback_data, const char* uri) {
	for (uint32_t i=0; i < urimap_len; ++i) {
		if (!strcmp(urimap[i], uri)) {
			//printf("Found mapped URI '%s' -> %d\n", uri, i);
			return i;
		}
	}
	//printf("map URI '%s' -> %d\n", uri, urimap_len);
	urimap = (char**) realloc(urimap, (urimap_len + 1) * sizeof(char*));
	urimap[urimap_len] = strdup(uri);
	return urimap_len++;
}

static void free_uri_map() {
	for (uint32_t i=0; i < urimap_len; ++i) {
		free(urimap[i]);
	}
	free(urimap);
}

void write_function(
		LV2UI_Controller controller,
		uint32_t         port_index,
		uint32_t         buffer_size,
		uint32_t         port_protocol,
		const void*      buffer) {

	if (buffer_size == 0) return;

	if (port_protocol != 0) {
		if (jack_ringbuffer_write_space(rb_atom_from_ui) >= buffer_size + sizeof(LV2_Atom)) {
			LV2_Atom a = {buffer_size, 0};
			jack_ringbuffer_write(rb_atom_from_ui, (char *) &a, sizeof(LV2_Atom));
			jack_ringbuffer_write(rb_atom_from_ui, (char *) buffer, buffer_size);
		}
		return;
	}
	if (buffer_size != sizeof(float)) {
		fprintf(stderr, "LV2Host: write_function() unsupported buffer\n");
		return;
	}
	if (port_index >=0 && port_index < nports_total && portmap_ctrl[port_index] < 0) {
		fprintf(stderr, "LV2Host: write_function() unmapped port\n");
		return;
	}
	if (jack_ringbuffer_write_space(rb_ctrl_from_ui) >= sizeof(uint32_t) + sizeof(float)) {
		jack_ringbuffer_write(rb_ctrl_from_ui, (char *) &portmap_ctrl[port_index], sizeof(uint32_t));
		jack_ringbuffer_write(rb_ctrl_from_ui, (char *) buffer, sizeof(float));
	}
}


/******************************************************************************
 * MAIN
 */

static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client=NULL;
  }

	if (plugin_dsp && plugin_instance && plugin_dsp->deactivate) {
		plugin_dsp->deactivate(plugin_instance);
	}
	if (plugin_gui && gui_instance && plugin_gui->cleanup) {
		plugin_gui->cleanup(gui_instance);
	}
	if (plugin_dsp && plugin_instance && plugin_dsp->cleanup) {
		plugin_dsp->cleanup(plugin_instance);
	}

	jack_ringbuffer_free(rb_ctrl_to_ui);
	jack_ringbuffer_free(rb_ctrl_from_ui);

	jack_ringbuffer_free(rb_atom_to_ui);
	jack_ringbuffer_free(rb_atom_from_ui);

  free(input_port);
  free(output_port);

	free(plugin_ports_pre);
	free(plugin_ports_post);
	free(portmap_a_in);
	free(portmap_a_out);
	free(portmap_ctrl);
	free(portmap_rctl);
	free_uri_map();
  fprintf(stderr, "bye.\n");
}

static void main_loop(void) {
	struct timespec timeout;
	LV2_Atom_Sequence *data = (LV2_Atom_Sequence*) malloc(min_atom_bufsiz * sizeof(uint8_t));

  pthread_mutex_lock (&gui_thread_lock);
  while (client_state != Exit) {

		while (jack_ringbuffer_read_space(rb_ctrl_to_ui) >= sizeof(uint32_t) + sizeof(float)) {
			uint32_t idx;
			float val;
			jack_ringbuffer_read(rb_ctrl_to_ui, (char*) &idx, sizeof(uint32_t));
			jack_ringbuffer_read(rb_ctrl_to_ui, (char*) &val, sizeof(float));
			plugin_gui->port_event(gui_instance, idx, sizeof(float), 0, &val);
		}

		while (jack_ringbuffer_read_space(rb_atom_to_ui) > sizeof(LV2_Atom)) {
			LV2_Atom a;
			jack_ringbuffer_read(rb_atom_to_ui, (char *) &a, sizeof(LV2_Atom));
			assert(a.size < min_atom_bufsiz);
			jack_ringbuffer_read(rb_atom_to_ui, (char *) data, a.size);
			LV2_Atom_Event const* ev = (LV2_Atom_Event const*)((&(data)->body) + 1); // lv2_atom_sequence_begin
			while((const uint8_t*)ev < ((const uint8_t*) &(data)->body + (data)->atom.size)) {
				plugin_gui->port_event(gui_instance, portmap_atom_to_ui,
						ev->body.size, uri_atom_EventTransfer, &ev->body);
				ev = (LV2_Atom_Event const*) /* lv2_atom_sequence_next() */
					((const uint8_t*)ev + sizeof(LV2_Atom_Event) + ((ev->body.size + 7) & ~7));
			}
		}

		LV2_EXTERNAL_UI_RUN(extui);

    if (client_state == Exit) break;

		clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_nsec += 1000000000 / (UI_UPDATE_FPS);
		if (timeout.tv_nsec >= 1000000000) {timeout.tv_nsec -= 1000000000; timeout.tv_sec+=1;}
    pthread_cond_timedwait (&data_ready, &gui_thread_lock, &timeout);

  } /* while running */
	free(data);
  pthread_mutex_unlock (&gui_thread_lock);
}

static void catchsig (int sig) {
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state=Exit;
  pthread_cond_signal (&data_ready);
}

static void on_external_ui_closed(void* controller) {
	catchsig(0);
}

int main (int argc, char **argv) {
	uint32_t c_ain  = 0;
	uint32_t c_aout = 0;
	uint32_t c_ctrl = 0;

	LV2_URID_Map uri_map            = { NULL, &uri_to_id };
	const LV2_Feature map_feature   = { LV2_URID__map, &uri_map};
	const LV2_Feature unmap_feature = { LV2_URID__unmap, NULL };

	const LV2_Feature* features[] = {
		&map_feature, &unmap_feature, NULL
	};

	const LV2_Feature external_lv_feature = { LV2_EXTERNAL_UI_URI, &extui_host};
	const LV2_Feature external_kx_feature = { LV2_EXTERNAL_UI_URI__KX__Host, &extui_host};
	LV2_Feature instance_feature          = { "http://lv2plug.in/ns/ext/instance-access", NULL };

	const LV2_Feature* ui_features[] = {
		&map_feature, &unmap_feature,
		&instance_feature,
		&external_lv_feature,
		&external_kx_feature,
		NULL
	};

	/* check sourced settings */
	assert ((nports_midi_in + nports_atom_in) <= 1);
	assert ((nports_midi_out + nports_atom_out) <= 1);
	assert (plugin_human_id);
	assert (nports_total > 0);

	extui_host.plugin_human_id = plugin_human_id;

	// TODO check if allocs succeeded - OOM -> exit
	/* allocate data structure */
	portmap_a_in  = (uint32_t*) malloc(nports_audio_in * sizeof(uint32_t));
	portmap_a_out = (uint32_t*) malloc(nports_audio_out * sizeof(uint32_t));
	portmap_rctl  = (uint32_t*) malloc(nports_ctrl  * sizeof(uint32_t));
	portmap_ctrl  = (int*)      malloc(nports_total * sizeof(int));

	plugin_ports_pre  = (float*) calloc(nports_ctrl, sizeof(float));
	plugin_ports_post = (float*) calloc(nports_ctrl, sizeof(float));

	atom_in = (LV2_Atom_Sequence*) malloc(min_atom_bufsiz + sizeof(uint8_t));
	atom_out = (LV2_Atom_Sequence*) malloc(min_atom_bufsiz + sizeof(uint8_t));

	rb_ctrl_to_ui = jack_ringbuffer_create((UPDATE_FREQ_RATIO) * nports_ctrl * 2 * sizeof(float));
	rb_ctrl_from_ui = jack_ringbuffer_create((UPDATE_FREQ_RATIO) * nports_ctrl * 2 * sizeof(float));

	rb_atom_to_ui = jack_ringbuffer_create((UPDATE_FREQ_RATIO) * min_atom_bufsiz);
	rb_atom_from_ui = jack_ringbuffer_create((UPDATE_FREQ_RATIO) * min_atom_bufsiz);


	/* reolve descriptors */
	plugin_dsp = lv2_descriptor(dsp_descriptor_id);
	plugin_gui = lv2ui_descriptor(gui_descriptor_id);

	if (!plugin_dsp) {
		fprintf(stderr, "cannot resolve LV2 descriptor\n");
		goto out;
	}
	/* jack-open -> samlerate */
  if (init_jack(extui_host.plugin_human_id)) goto out;

	/* init plugin */
	plugin_instance = plugin_dsp->instantiate(plugin_dsp, j_samplerate, NULL, features);
	if (!plugin_instance) {
		fprintf(stderr, "instantiation failed\n");
		goto out;
	}

	/* connect ports */
	for (uint32_t p=0; p < nports_total; ++p) {
		portmap_ctrl[p] = -1;
		switch (ports[p].porttype) {
			case CONTROL_IN:
				plugin_ports_pre[c_ctrl] = ports[p].val_default;
			case CONTROL_OUT:
				portmap_ctrl[p] = c_ctrl;
				portmap_rctl[c_ctrl] = p;
				plugin_dsp->connect_port(plugin_instance, p , &plugin_ports_pre[c_ctrl++]);
				break;
			case AUDIO_IN:
				portmap_a_in[c_ain++] = p;
				break;
			case AUDIO_OUT:
				portmap_a_out[c_aout++] = p;
				break;
			case MIDI_IN:
			case ATOM_IN:
				portmap_atom_from_ui = p;
				plugin_dsp->connect_port(plugin_instance, p , atom_in);
				break;
			case MIDI_OUT:
			case ATOM_OUT:
				portmap_atom_to_ui = p;
				plugin_dsp->connect_port(plugin_instance, p , atom_out);
				break;
			default:
				fprintf(stderr, "yet unsupported port..\n");
				break;
		}
	}

	assert(c_ain == nports_audio_in);
	assert(c_aout == nports_audio_out);
	assert(c_ctrl == nports_ctrl);

	if (nports_atom_out > 0 || nports_atom_in > 0 || nports_midi_in > 0 || nports_midi_out > 0) {
		uri_atom_Sequence       = uri_to_id(NULL, LV2_ATOM__Sequence);
		uri_atom_EventTransfer  = uri_to_id(NULL, LV2_ATOM__eventTransfer);
		uri_midi_MidiEvent      = uri_to_id(NULL, LV2_MIDI__MidiEvent);
		uri_time_Position       = uri_to_id(NULL, LV2_TIME__Position);
		uri_time_frame          = uri_to_id(NULL, LV2_TIME__frame);
		uri_time_speed          = uri_to_id(NULL, LV2_TIME__speed);
		uri_time_bar            = uri_to_id(NULL, LV2_TIME__bar);
		uri_time_barBeat        = uri_to_id(NULL, LV2_TIME__barBeat);
		uri_time_beatUnit       = uri_to_id(NULL, LV2_TIME__beatUnit);
		uri_time_beatsPerBar    = uri_to_id(NULL, LV2_TIME__beatsPerBar);
		uri_time_beatsPerMinute = uri_to_id(NULL, LV2_TIME__beatsPerMinute);
		lv2_atom_forge_init(&lv2_forge, &uri_map);
	}

  if (jack_portsetup()) goto out;

	if (plugin_gui) {
	/* init plugin GUI */
	extui_host.ui_closed = on_external_ui_closed;
	instance_feature.data = plugin_instance;
	gui_instance = plugin_gui->instantiate(plugin_gui,
			plugin_dsp->URI, NULL,
			&write_function, controller,
			(void **)&extui, ui_features);

	}

#ifdef REQUIRE_UI
	if (!gui_instance || !extui) {
    fprintf(stderr, "Error: GUI was not initialized.\n");
		goto out;
	}
#endif

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

	if (plugin_dsp->activate) {
		plugin_dsp->activate(plugin_instance);
	}

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

	if (!gui_instance || !extui) {
		/* no GUI */
		while (client_state != Exit) {
			sleep (1);
		}
	} else {

		LV2_EXTERNAL_UI_SHOW(extui);

		main_loop();

		LV2_EXTERNAL_UI_HIDE(extui);
	}

out:
  cleanup(0);
  return(0);
}
/* vi:set ts=2 sts=2 sw=2: */
