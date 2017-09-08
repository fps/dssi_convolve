/*
    Copyright (C) 2004 Florian Schmidt
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software 
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
    This plugin is based on the example code by steve harris from the
    dssi distribution
*/

#include <assert.h>

#include <pthread.h>

#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <convolve.h>
#include <sndfile.h>
#include <samplerate.h>

#include "dssi.h"

/*	defaults.h gives us i.e. DEFAULT_PARTITION_SIZE */
#include "defaults.h"

/* 
	he macro CHANNELS is defined by the makefile. i.e. CHANNELS=2 
	to create different versions of the plugin.

	As is the LADSPA plugin ID (macro LADSPA_ID).
	each plugin gets its own.
*/

/* 	symbolic names for the ladspa port numbers */
#define CONVOLVE_CONTROL_GAIN     0
#define CONVOLVE_CONTROL_DRY_GAIN 1
#define CONVOLVE_CONTROL_LATENCY  2

/* 	we need above plus input and output */
#define CONVOLVE_NUM_OF_PORTS    (3 + 2 * CHANNELS)

/* 
	some convenience macros to make finding
	the right port easier
*/
#define CONVOLVE_INPUT_START     3
#define CONVOLVE_OUTPUT_START    (3 + CHANNELS)

#define MAX_STRINGLENGTH 1024

#define MIN(a, b) (a < b ? a : b)

static LADSPA_Descriptor *convolveLDescriptor = NULL;
static DSSI_Descriptor   *convolveDDescriptor = NULL;


/* 
	this is the data that each instance has to its avail. all
	state must be stored in this struct
*/
typedef struct 
{
	/* Audio Ports */
	LADSPA_Data     **outputs;
	LADSPA_Data     **inputs;

	/* Control Ports */
	LADSPA_Data     *control_gain;
	LADSPA_Data     *control_dry_gain;
	LADSPA_Data     *control_latency;

	unsigned int     samplerate;

	/* The convolution data structore from libconvolve */
	convolution_t    conv;

	int              file_loaded;
	char            *filename;

	volatile int     conv_thread_ready;

	unsigned int     partition_size;
	int              realtime;

	/* the convolution slave thread and synchronization tools */
	pthread_t       *conv_thread;
	pthread_cond_t  *conv_condition;
	pthread_mutex_t *conv_mutex;

	pthread_mutex_t *main_mutex;

	volatile int     done;
	volatile int     work_to_do;

	/* buffers for the double buffering. for the input */
	float          **input_buffers;
	/* and as many as channels for output */
	float          **output_buffers;

	/* these get tweaked to point to the right half of the buffers: */
	/* these are for the positions that get written to/read from by the conv thread: */
	float          **current_conv_input_ptrs;
	float          **current_conv_output_ptrs;
	
	/* index for the main thread to keep track where it's reading/writing */
	unsigned int     main_buffer_index;

} Convolve;


/* this is the worker thread routine */
void *conv_thread_routine(void *arg) 
{
	Convolve           *plugin;
	struct sched_param  params;
	int                 policy;
	int                 index,
	                    index2;

	printf("[dssi_convolve]: conv thread running\n");
	
	plugin = (Convolve*)arg;

	if (pthread_getschedparam(pthread_self(), &policy, &params) != 0) 
		printf("[dssi_convolve]: conv thread: couldn't get thread priority\n");
	if (policy == SCHED_FIFO) 
		printf("[dssi_convolve]: conv thread: we be SCHED_FIFO\n");
	if (policy == SCHED_OTHER) 
		printf("[dssi_convolve]: conv thread: we be SCHED_OTHER\n");

	plugin->conv_thread_ready = 1;
	while(!plugin->done) {
		/* this thread basically alwasy sleeps until the
		   main thread has enough data collected (plugin->partition_size).
		   then we get to run on this data */
		if (pthread_mutex_lock(plugin->conv_mutex) != 0) {
			printf("[dssi_convolve]: conv thread: error locking mutex\n");
			return 0;
		}

		if (pthread_cond_wait(plugin->conv_condition, plugin->conv_mutex) != 0) {
			printf("[dssi_convolve]: conv thread: error waiting on condition\n");
			return 0;
		}

		if (plugin->done) {
			if (pthread_mutex_unlock(plugin->conv_mutex) != 0) {
				printf("[dssi_convolve]: error unlocking mutex\n");
				return 0;
			}
			printf("[dssi_convolve]: conv thread done.\n");
			return 0;
		}

	
		/* check for spurious wakeup */
		if (plugin->work_to_do) {
			plugin->work_to_do = 0;

			if (pthread_mutex_unlock(plugin->conv_mutex) != 0) {
				printf("[dssi_convolve]: error unlocking mutex\n");
				return 0;
			}

			// printf("[dssi_convolve]: processing %f\n", plugin->current_conv_input_ptrs[0][0]);
			convolution_process(&plugin->conv, plugin->current_conv_input_ptrs, plugin->current_conv_output_ptrs, 1.0, 0, plugin->partition_size);

			for (index = 0; index < CHANNELS; ++index) {
				for (index2 = 0; index2 < plugin->partition_size; ++index2) {
					plugin->current_conv_output_ptrs[index][index2] = 
						  (*plugin->control_dry_gain * (plugin->current_conv_input_ptrs[index][index2]))
						+ (*plugin->control_gain * (plugin->current_conv_output_ptrs[index][index2]));
				}
			}
		}

	}
	printf("[dssi_convolve]: conv thread done.\n");
	return 0;
}


/* accessor functions which will be called by the host to get
   our descriptors:
   LADSPA Descriptor */
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
	switch (index) {
		case 0:
			return convolveLDescriptor;
		default:
			return NULL;
	}
}

/* DSSI Descriptor */
const DSSI_Descriptor *dssi_descriptor(unsigned long index)
{
	switch (index) {
		case 0:
			return convolveDDescriptor;
		default:
			return NULL;
	}
}


static void cleanupConvolve(LADSPA_Handle instance)
{
	Convolve *plugin;
	int       index;

	/* end worker thread */
	plugin = (Convolve *)instance;
	if(pthread_mutex_lock(plugin->conv_mutex) != 0) 
		printf("[dssi_convolve]: error locking mutex\n");	

	plugin->done = 1;

	if (pthread_cond_signal(plugin->conv_condition) != 0)
		printf("[dssi_convolve]: error sending signal\n");

	if (pthread_mutex_unlock(plugin->conv_mutex) != 0)
		printf("[dssi_convolve]: error unlocking mutex\n");

	/* wait till it's really done */
	if (pthread_join(*plugin->conv_thread, 0) != 0) 
		printf("[dssi_convolve]: error joining conv thread\n");


	printf("[dssi-convolve]: cleanup: conv thread finished\n");

	if (plugin->file_loaded) {
		for (index = 0; index < CHANNELS; ++index) 
			free(plugin->output_buffers[index]);

		for (index = 0; index < CHANNELS; ++index) 
			free(plugin->input_buffers[index]);

		convolution_destroy(&plugin->conv);
		free(plugin->current_conv_output_ptrs);
		free(plugin->current_conv_input_ptrs);
	}

	free(plugin->inputs);
	free(plugin->outputs);	

	free(plugin->filename);

	free(plugin->conv_thread);
	free(plugin->conv_condition);
	free(plugin->conv_mutex);
	free(plugin->main_mutex);


    free(plugin);
}


static void connectPortConvolve(LADSPA_Handle instance, unsigned long port,
			  LADSPA_Data *data)
{
	Convolve *plugin;

	assert(port < CONVOLVE_INPUT_START + 2 * CHANNELS);
	plugin = (Convolve*)instance;
	if (port < CONVOLVE_INPUT_START) {
		switch (port) {
		case CONVOLVE_CONTROL_LATENCY:
			plugin->control_latency = data;
			break;
		
		case CONVOLVE_CONTROL_GAIN:
			plugin->control_gain = data;
			break;

		case CONVOLVE_CONTROL_DRY_GAIN:
			plugin->control_dry_gain = data;
			break;

		default:
			break;
		}
	} else if ((port >= CONVOLVE_INPUT_START) && (port < CONVOLVE_OUTPUT_START)) {
		plugin->inputs[port - CONVOLVE_INPUT_START] = data;
	} else if (port >= CONVOLVE_OUTPUT_START) {
		plugin->outputs[port - CONVOLVE_OUTPUT_START] = data;
	}
}


static LADSPA_Handle instantiateConvolve(const LADSPA_Descriptor * descriptor,
				   unsigned long s_rate)
{
	Convolve              *plugin;

	pthread_mutexattr_t    mutex_attr;

	plugin                 = (Convolve*)malloc(sizeof(Convolve));

	/* initially we do not have any file loaded */
	plugin->file_loaded    = 0;
	plugin->filename       = (char*)malloc(MAX_STRINGLENGTH);

	plugin->done           = 0;

	plugin->samplerate     = s_rate;
	plugin->partition_size = DEFAULT_PARTITION_SIZE;
 
	plugin->realtime       = 0;

	plugin->outputs        = (LADSPA_Data**)malloc(CHANNELS*sizeof(LADSPA_Data));
	plugin->inputs         = (LADSPA_Data**)malloc(CHANNELS*sizeof(LADSPA_Data));

	/* allocate space for current ptr's into buffers */
	plugin->current_conv_output_ptrs = (float**)malloc(sizeof(float*) * CHANNELS);
	plugin->current_conv_input_ptrs = (float**)malloc(sizeof(float*) * CHANNELS);

	pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
	// pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_FAST);

	plugin->conv_mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
	if (pthread_mutex_init(plugin->conv_mutex, &mutex_attr) != 0)
	//if (pthread_mutex_init(plugin->conv_mutex, 0) != 0)
		printf("[dssi_convolve]: error init mutex\n");

	plugin->main_mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
	if (pthread_mutex_init(plugin->main_mutex, &mutex_attr) != 0)
	//if (pthread_mutex_init(plugin->main_mutex, 0) != 0)
		printf("[dssi_convolve]: error init mutex\n");

	plugin->conv_condition = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));
	if (pthread_cond_init(plugin->conv_condition, 0) != 0)
		printf("[dssi_convolve]: error init cond\n");

	plugin->conv_thread_ready = 0;
	plugin->conv_thread = (pthread_t*)malloc(sizeof(pthread_t));
	if (pthread_create(plugin->conv_thread, 0, conv_thread_routine, plugin) != 0)
		printf("[dssi_convolve]: error creating thread\n");
	
    return (LADSPA_Handle)plugin;
}


static void activateConvolve(LADSPA_Handle instance)
{
    Convolve *plugin;

	plugin = (Convolve *) instance;
}

static void configure_rtprio(Convolve *plugin, const char *prio) 
{
	int                 rtprio;
	int                 policy;
	struct sched_param  params;

	if (pthread_getschedparam(*plugin->conv_thread, &policy, &params) != 0) {
		printf("[dssi_convolve]: couldn't get sched params\n");
	}
	
	rtprio = atoi(prio);
	if (rtprio != 0) 
		policy = SCHED_FIFO;
	else
		policy = SCHED_OTHER;

	params.sched_priority = rtprio;
	if (pthread_setschedparam(*plugin->conv_thread, policy, &params) != 0) {
		printf("[dssi_convolve]: couldn't set rtprio %i\n", rtprio);
	}
}

static void configure_responsefile(Convolve *plugin, const char* filename) 
{
	response_t     *response;
	unsigned int    index, index2;

	printf("[dssi_convolve]: loading responsefile: %s\n", filename);

	if (pthread_mutex_lock(plugin->conv_mutex) != 0) 
		printf("[dssi_convolve]: configure: error locking conv mutex\n");
	
	if (pthread_mutex_lock(plugin->main_mutex) != 0) 
		printf("[dssi_convolve]: configure: error locking main mutex\n");
	
	/* yep. so first we destroy the old convolution_t */
	if (plugin->file_loaded != 0) {
		convolution_destroy(&plugin->conv);
		plugin->file_loaded = 0;

		/* and also free the double buffers */
		for (index = 0; index < CHANNELS; ++index) {
			free(plugin->output_buffers[index]);
		}
		for (index = 0; index < CHANNELS; ++index) {
			free(plugin->input_buffers[index]);
		}
		free(plugin->output_buffers);
		free(plugin->input_buffers);
	
	}
	
	response = (response_t*)malloc(sizeof(response_t));

	if (!(load_response(response, (char*)filename, plugin->samplerate))) {

		pthread_mutex_unlock(plugin->main_mutex);
		pthread_mutex_unlock(plugin->conv_mutex);

		return;

	} else {

		if (response->channels != CHANNELS) {
			printf("[dssi_convolve]: channel mismatch\n");
			/* clean up */
			for (index = 0; index < response->channels; ++index) {
				free(response->channel_data[index]);
			}

			free(response->channel_data);
			free(response);

			pthread_mutex_unlock(plugin->main_mutex);
			pthread_mutex_unlock(plugin->conv_mutex);

			return;
		}
	}

	/* we use split channel mode here (argument 6 = 1) */
	printf("[dssi_convolve]: initializing convolution...");	fflush(stdout);
	convolution_init(&(plugin->conv), 1, CHANNELS, &response, plugin->partition_size, 1);
	printf("done\n");

	strncpy(plugin->filename, filename, MAX_STRINGLENGTH);
	plugin->file_loaded = 1;

	/* setup double buffering buffers :) */
	plugin->input_buffers = (float**)malloc(CHANNELS * sizeof(float*));
	plugin->output_buffers = (float**)malloc(CHANNELS * sizeof(float*));
	for (index = 0; index < CHANNELS; ++index) {
		plugin->input_buffers[index] = (float*)malloc(plugin->partition_size * 2 * sizeof(float));
		plugin->output_buffers[index] = (float*)malloc(plugin->partition_size * 2 * sizeof(float));
	}

	/* we should really initialize the double buffers with 0's */
	for (index = 0; index < CHANNELS; ++index) {
		for (index2 = 0; index2 < plugin->partition_size * 2; ++index2) {
			plugin->output_buffers[index][index2] = 0;
			plugin->input_buffers[index][index2] = 0;
		}
	}

	/* write to/read from lower buffer half initially */
	plugin->main_buffer_index = 0;

	for (index = 0; index < CHANNELS; ++index) {
		plugin->current_conv_input_ptrs[index] = plugin->input_buffers[index] + plugin->partition_size;	
		plugin->current_conv_output_ptrs[index] = plugin->output_buffers[index] + plugin->partition_size;
	}

	/* clean up response struct and all contents. it's not needed anymore */
	for (index = 0; index < CHANNELS; ++index) {
		free(response->channel_data[index]);
	}

	free(response->channel_data);
	free(response);

	/* now we're ready to go */
	pthread_mutex_unlock(plugin->main_mutex);
	pthread_mutex_unlock(plugin->conv_mutex);

	printf("[dssi_convolve]: ready\n");
}

static void configure_partitionsize(Convolve *plugin, const char *size) 
{
	int partition_size;
	pthread_mutex_lock(plugin->main_mutex);

	partition_size = atoi(size);
	printf("[dssi_convolve]: setting partitionsize: %i\n", partition_size);

	if(partition_size == plugin->partition_size)
		return;

	plugin->partition_size = partition_size;

	/*	if partition size changed we need to reload the response file if 
		there's one loaded
	*/
	if (plugin->file_loaded) 
		configure_responsefile(plugin, plugin->filename);
	
	pthread_mutex_unlock(plugin->main_mutex);
}


static char *configureConvolve(LADSPA_Handle instance, const char *key, const char *value) {
	Convolve       *plugin;

	plugin = (Convolve*)instance;

	printf("[dssi_convolve]: configure: %s %s\n", key, value);

	/* should we setup a new responsefile? */
	if (strncmp(key, "responsefile", 12) == 0) {
		configure_responsefile(plugin, value);
	}
	if (strncmp(key, "rtprio", 6) == 0) {
		configure_rtprio(plugin, value);
	}
	if (strncmp(key, "partitionsize", 13) == 0) {
		configure_partitionsize(plugin, value);
	}
	return 0;
}


static void runConvolve(LADSPA_Handle instance, unsigned long sample_count,
		  snd_seq_event_t *events, unsigned long event_count)
{
	Convolve    * plugin;
	LADSPA_Data **outputs;
	LADSPA_Data **inputs;
	LADSPA_Data   control_gain;
	LADSPA_Data   control_dry_gain;

	unsigned int  frames_left, 
	              in_index, 
	              tmp_frames, 
	              index, 
	              index2, 
	              ret;

	plugin            = (Convolve *)instance;
	outputs           = plugin->outputs;
	inputs            = plugin->inputs;
	control_gain      = *(plugin->control_gain);
	control_dry_gain  = *(plugin->control_dry_gain);

	if ((ret=pthread_mutex_trylock(plugin->main_mutex)) != 0 || !plugin->file_loaded) {
		/* fill output with 0's if not */
		/* printf("[dssi_convolve]: feeding 0's\n"); */
		for (index = 0; index < CHANNELS; ++index) {
			for (index2 = 0; index2 < sample_count; ++index2) {
				outputs[index][index2] = 0;
			}
		}
		if(ret == 0)
			pthread_mutex_unlock(plugin->main_mutex);

		return;
	}

	*(plugin->control_latency) = 2 * plugin->partition_size;	

#if 0
	/* are we initialized? */
	if(!plugin->file_loaded) {
		/* fill output with 0's if not */
		for (index = 0; index < sample_count; ++index) {
			left_output[index] = 0;
			right_output[index] = 0;
		}
		return;
	}
#endif

	/* printf("[dssi_convolve]: ."); fflush(stdout); */

	/* ok, do that buffering/threading thang */
	frames_left = sample_count;
	in_index = 0;

	while (frames_left > 0) {
		/* read/consume from main half of double buffers */
		tmp_frames = MIN(frames_left, 
		                 plugin->partition_size 
		              - (plugin->main_buffer_index % plugin->partition_size));

		for (index = 0; index < CHANNELS; ++index) {
			for (index2 = 0; index2 < tmp_frames; ++index2) {
				plugin->input_buffers[index][plugin->main_buffer_index + index2] = 
					inputs[index][index2];
#if 0
				outputs[index][index2] = 
					  (control_dry_gain * plugin->input_buffers[index][index2 + ((plugin->main_buffer_index + plugin->partition_size)) % (2 * plugin->partition_size)])
					+ (control_gain * plugin->output_buffers[index][index2 + plugin->main_buffer_index]);
#endif
				outputs[index][index2] = plugin->output_buffers[index][index2 + plugin->main_buffer_index];
			}
		}
		frames_left -= tmp_frames;
		plugin->main_buffer_index += tmp_frames;

		/* [a] */
		plugin->main_buffer_index %= (2 * plugin->partition_size);

		/* [b] */
		if (plugin->main_buffer_index % plugin->partition_size == 0) {
			/* setup current_conv_ptrs and... */
			if (pthread_mutex_trylock(plugin->conv_mutex) == 0) {
				if (plugin->main_buffer_index == 0) {
					/* if the main_bufer_index wrapped to 0, we have to point the
					   convolution to the upper half of the buffer */

					for (index = 0; index < CHANNELS; ++index) {
						plugin->current_conv_input_ptrs[index] = plugin->input_buffers[index] + plugin->partition_size;	
						plugin->current_conv_output_ptrs[index] = plugin->output_buffers[index] + plugin->partition_size;
					}

				} else {
					/* the other case. here the main_buffer_index is at
					   plugin->partition_size because of [a] and [b] */

					for (index = 0; index < CHANNELS; ++index) {
						plugin->current_conv_input_ptrs[index] = plugin->input_buffers[index];
						plugin->current_conv_output_ptrs[index] = plugin->output_buffers[index];
					}
				}
				/* ...signal worker thread */
				plugin->work_to_do = 1;
				/* printf("[dssi_convolve]: x,"); fflush(stdout); */
				pthread_cond_signal(plugin->conv_condition);
				pthread_mutex_unlock(plugin->conv_mutex);
			}
		}
	}
	pthread_mutex_unlock(plugin->main_mutex);
}


static void runConvolveWrapper(LADSPA_Handle instance,
			 unsigned long sample_count)
{
    runConvolve(instance, sample_count, NULL, 0);
}


/* this function gets automagically called by the dynamic linker. here
   we setup the LADSPA_Descriptor and the DSSI_Descriptor that 
   the ladspa_descriptor() and dssi_descriptor functions return
*/
void _init()
{
	char                  **port_names;
	LADSPA_PortDescriptor  *port_descriptors;
	LADSPA_PortRangeHint   *port_range_hints;
	int                     index;
	char                   *tmp_portname;	

	convolveLDescriptor = (LADSPA_Descriptor*)malloc(sizeof(LADSPA_Descriptor));
	if (convolveLDescriptor) {

		convolveLDescriptor->UniqueID = LADSPA_ID + CHANNELS - 1;
		convolveLDescriptor->Label = NAME;
		convolveLDescriptor->Properties = 0;
		convolveLDescriptor->Name = "DSSI-" NAME;
		convolveLDescriptor->Maker = "Florian Schmidt <tapas@affenbande.org>";
		convolveLDescriptor->Copyright = "GPL";
		convolveLDescriptor->PortCount = CONVOLVE_NUM_OF_PORTS;

		/* allocate space for the port descriptors */
		port_descriptors = (LADSPA_PortDescriptor *)calloc(convolveLDescriptor->PortCount, 
		                                                   sizeof(LADSPA_PortDescriptor));
		convolveLDescriptor->PortDescriptors = (const LADSPA_PortDescriptor *) port_descriptors;

		/* allocate space for the port range hints */		
		port_range_hints = (LADSPA_PortRangeHint *) calloc(convolveLDescriptor->PortCount, 
		                                                   sizeof(LADSPA_PortRangeHint));
		convolveLDescriptor->PortRangeHints = (const LADSPA_PortRangeHint *) port_range_hints;

		/* allocate space for the port names */
		port_names = (char **) calloc(convolveLDescriptor->PortCount, sizeof(char *));
		convolveLDescriptor->PortNames = (const char **) port_names;

		/* Parameters for inputs */
		for (index = CONVOLVE_INPUT_START; index < CONVOLVE_OUTPUT_START; ++index) {
			port_descriptors[index] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
			tmp_portname = (char*)malloc(20);
			sprintf(tmp_portname, "input%i", index - CONVOLVE_INPUT_START);
			port_names[index] = tmp_portname;
			port_range_hints[index].HintDescriptor = 0;
		}

		/* Parameters for Control Gain */
		port_descriptors[CONVOLVE_CONTROL_GAIN] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
		port_names[CONVOLVE_CONTROL_GAIN] = "gain";
		port_range_hints[CONVOLVE_CONTROL_GAIN].HintDescriptor = LADSPA_HINT_DEFAULT_1 | 
		                                                         LADSPA_HINT_BOUNDED_BELOW | 
		                                                         LADSPA_HINT_BOUNDED_ABOVE;

		port_range_hints[CONVOLVE_CONTROL_GAIN].LowerBound = 0.0;
		port_range_hints[CONVOLVE_CONTROL_GAIN].UpperBound = 20.0;

		/* Parameters for Control Dry Gain */
		port_descriptors[CONVOLVE_CONTROL_DRY_GAIN] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
		port_names[CONVOLVE_CONTROL_DRY_GAIN] = "dry_gain";
		port_range_hints[CONVOLVE_CONTROL_DRY_GAIN].HintDescriptor = LADSPA_HINT_DEFAULT_0 | 
		                                                         LADSPA_HINT_BOUNDED_BELOW | 
		                                                         LADSPA_HINT_BOUNDED_ABOVE;

		port_range_hints[CONVOLVE_CONTROL_DRY_GAIN].LowerBound = 0.0;
		port_range_hints[CONVOLVE_CONTROL_DRY_GAIN].UpperBound = 20.0;

		/* Parameters for outputs */
		for (index = CONVOLVE_OUTPUT_START; index < CONVOLVE_NUM_OF_PORTS; ++index) {
			port_descriptors[index] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
			tmp_portname = (char*)malloc(20);
			sprintf(tmp_portname, "output%i", index - CONVOLVE_OUTPUT_START);
			port_names[index] = tmp_portname;
			port_range_hints[index].HintDescriptor = 0;
		}

		/* Parameters for Control Gain */
		port_descriptors[CONVOLVE_CONTROL_LATENCY] = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
		port_names[CONVOLVE_CONTROL_LATENCY] = "latency";
		port_range_hints[CONVOLVE_CONTROL_LATENCY].HintDescriptor = 0; 

		/* LADSPA functions */
		convolveLDescriptor->activate = activateConvolve;
		convolveLDescriptor->cleanup = cleanupConvolve;
		convolveLDescriptor->connect_port = connectPortConvolve;
		convolveLDescriptor->deactivate = NULL;
		convolveLDescriptor->instantiate = instantiateConvolve;
		convolveLDescriptor->run = runConvolveWrapper;
		convolveLDescriptor->run_adding = NULL;
		convolveLDescriptor->set_run_adding_gain = NULL;
    }
	
    convolveDDescriptor = (DSSI_Descriptor*)malloc(sizeof(DSSI_Descriptor));
	if (convolveDDescriptor) {
		/* DSSI functions */
		convolveDDescriptor->DSSI_API_Version = 1;
		convolveDDescriptor->LADSPA_Plugin = convolveLDescriptor;
		convolveDDescriptor->configure = configureConvolve;
		convolveDDescriptor->get_program = NULL;
		convolveDDescriptor->get_midi_controller_for_port = NULL;
		convolveDDescriptor->select_program = NULL;
		convolveDDescriptor->run_synth = runConvolve;
		convolveDDescriptor->run_synth_adding = NULL;
		convolveDDescriptor->run_multiple_synths = NULL;
		convolveDDescriptor->run_multiple_synths_adding = NULL;
	}	
}


void _fini()
{
	if (convolveLDescriptor) {
		free((LADSPA_PortDescriptor *) convolveLDescriptor->PortDescriptors);
		free((char **) convolveLDescriptor->PortNames);
		free((LADSPA_PortRangeHint *) convolveLDescriptor->PortRangeHints);
		free(convolveLDescriptor);
	}
	if (convolveDDescriptor) {
		free(convolveDDescriptor);
	}
}
