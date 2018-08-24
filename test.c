#include <fluidsynth.h>
#include <stdlib.h>
#include <wiringPi.h>
#include <math.h>

#define abs(x) ((x) < 0 ? -(x) : (x))
#define set1bit(x, i) ((x) |= (1<<(i)))

struct fx_data_t {
	fluid_synth_t *synth;
	float val;
};

int instrument = 0;
fluid_settings_t* settings = NULL;
fluid_synth_t* synth = NULL;
fluid_sfont_t* sfont = NULL;
fluid_sequencer_t* seq;
short synth_destination, client_destination;

fluid_audio_driver_t* adriver = NULL;
fluid_midi_driver_t* mdriver = NULL;
fluid_player_t* dplayer = NULL;
const int CMajor[7] = {60, 62, 64, 65, 67, 69, 71};
const char const *NOTE_NAME[12] = {
	"C", "C#/Db", "D", "D#/Eb", 
	"E", "F", "F#/Gb", "G", "G#/Ab",
	"A", "A#/Bb", "B"
};

void showInst(int preNum) {
	fluid_preset_t* ps = sfont->get_preset(sfont, 0, preNum);
	printf("current instrument: No.%d: %s\n", preNum, 
			ps->get_name(ps));
}

int distortion = 0;
int fx_func(void *data, int len, int nin, float **in, int nout, float **out) {
	struct fx_data_t *fx_data = (struct fx_data_t*) data;
	if (fluid_synth_process(fx_data->synth, len, nin, in, nout, out) != 0) {
		return -1;
	}
	if (distortion) {
		for (int i = 0; i < nout; ++i) {
			float *out_i = out[i];
			for (int k = 0; k < len; ++k) {
				float x = out_i[k] * 20;
				
				if (x > 0) out_i[k] = 1 - exp(-x);
				else out_i[k] = -1 + exp(x);

				out_i[k] /= 4;

				/*
				if (abs(x) < 1e-8) continue;
				float y = x * x / abs(x);
				out_i[k] = x / abs(x) * (1 - exp(-y));
				*/
				// printf("%.2f, %.2f ", x, out_i[k]);
			}
		}
	}
	return 0;
}

struct cell {
	int on;
	int chan;
	int key;
	int vel;
	unsigned int time;
} sequence[200];
int recording = 0, total = 0;
void recordingControl(fluid_midi_event_t *evt) {
	sequence[total].on = (fluid_midi_event_get_type(evt) == 144 ? 1 : 0);
	sequence[total].chan = fluid_midi_event_get_channel(evt);
	sequence[total].key = fluid_midi_event_get_key(evt);
	sequence[total].vel = fluid_midi_event_get_velocity(evt);
	sequence[total].time = fluid_sequencer_get_tick(seq);
	++total;
}

#define CHORD_NUM 4
const int CHORD[CHORD_NUM] = { 1168, 144, 136, 72 };
const char const *CHORD_NAME[CHORD_NUM] = {
	"dominant seventh",
	"major",
	"minor",
	"diminished",
};
const int SCALE[CHORD_NUM][7] = {
	{2, 4, 5, 7, 9, 11},
	{2, 3, 5, 7, 8, 10},
};
int pitchs[130] = {0}, pcnt = 0;

void detectChord() {
	if (pcnt < 3) return;
	int oct[12] = {0}; // notes in one octave
	int flag[12] = {0}; // notes appearance flag
	int cnt = 0;
	for (int i = 0; i < 130; ++i) {
		if (pitchs[i] && !flag[i % 12]) {
			oct[cnt++] = i;
			flag[i % 12] = 1;
		}
	}
	for (int i = 1; i < cnt; ++i) { // shift notes to one octave
		while (oct[i] - 12 >= oct[0]) oct[i] -= 12;
	}

	int itv = 0; // intervals
	for (int i = 1; i < cnt; ++i) {
		set1bit(itv, oct[i] - oct[0]);
	}
	for (int c = 0; c < CHORD_NUM; ++c) {
		int chord = CHORD[c];
		if (itv == chord) {
			printf("%s %s\n", NOTE_NAME[oct[0] % 12], CHORD_NAME[c]);
			return;
		}
		else if ((itv & chord) == chord) {
			printf("partial %s %s\n", NOTE_NAME[oct[0] % 12], CHORD_NAME[c]);
			return;
		}
	}
}

int midiControl (void* data, fluid_midi_event_t* event) {
	fluid_synth_handle_midi_event(data, event);
	int type = fluid_midi_event_get_type(event), pitch = fluid_midi_event_get_pitch(event);
	if (type == 144 && pitchs[pitch] == 0) {
		pitchs[pitch] = 1;
		++pcnt;
		detectChord();
		if (recording) recordingControl(event);
	}
	else if (type == 128 && pitchs[pitch] == 1) {
		pitchs[pitch] = 0;
		--pcnt;
		if (recording) recordingControl(event);
	}
	else if (type == 192) {
		instrument = fluid_midi_event_get_control(event);
		showInst(instrument);
	}
	// printf("%d %d %d\n", pcnt, type, pitch);
	// printf("%u\n", fluid_sequencer_get_tick(seq));
	return 0;
}

int looping = 0;
void drumControl() {
	if (!digitalRead(7)) {
		if (looping) {
			// printf("before: %d\n", fluid_player_get_status(dplayer));
			fluid_player_stop(dplayer);
			// printf("after: %d\n", fluid_player_get_status(dplayer));
			looping = 0;
		}
		else {
			// printf("before: %d\n", fluid_player_get_status(dplayer));
			fluid_player_play(dplayer);
			// printf("after: %d\n", fluid_player_get_status(dplayer));
			looping = 1;
		}
		delay(100);
	}
}

void schedule_note(int chan, short key, short vel, unsigned int time, int on) {
	fluid_event_t *ev = new_fluid_event();
	fluid_event_set_source(ev, -1);
	fluid_event_set_dest(ev, synth_destination);
	if (on) fluid_event_noteon(ev, chan, key, vel);
	else fluid_event_noteoff(ev, chan, key);
	fluid_sequencer_send_at(seq, ev, time, 0);
	delete_fluid_event(ev);
}
void seq_callback(unsigned int time, fluid_event_t *evt, fluid_sequencer_t *seq, void *data) {
	for (int i = 0; i < total; ++i) {
		schedule_note(sequence[i].chan, sequence[i].key, sequence[i].vel, sequence[i].time, sequence[i].on);
	}
}

int main(int argc, char** argv)
{
	struct fx_data_t fx_data;
	int sfont_id;
	int ret;

	/* initialize wiringPi and pins */
	wiringPiSetup();
	for (int i = 0; i <= 7; ++i) {
		pinMode(i, INPUT);
		pullUpDnControl(i, PUD_UP);
	}
	for (int i = 21; i <= 25; ++i) {
		pinMode(i, INPUT);
		pullUpDnControl(i, PUD_UP);
	}

	/* Create the settings. */
	settings = new_fluid_settings();
	
	/* Change the settings if necessary*/
	ret = fluid_settings_setstr(settings, "audio.driver", "alsa");
	if (ret == FLUID_FAILED) {
		fprintf(stderr, "error on setting audio driver\n");
		goto cleanUp;
	}
	ret = fluid_settings_setnum(settings, "synth.gain", 2.0);
	if (ret == FLUID_FAILED) {
		fprintf(stderr, "error on setting gain\n");
		goto cleanUp;
	}
	ret = fluid_settings_setstr(settings, "midi.driver", "alsa_raw");
	if (ret == FLUID_FAILED) {
		fprintf(stderr, "error on setting midi driver\n");
		goto cleanUp;
	}
	ret = fluid_settings_setstr(settings, "midi.alsa.device", "hw:1,0,0");
	if (ret == FLUID_FAILED) {
		fprintf(stderr, "error on setting midi device\n");
		goto cleanUp;
	}
	
	/* Create the synthesizer. */
	synth = new_fluid_synth(settings);
	
	fx_data.synth = synth;

	mdriver = new_fluid_midi_driver(settings, midiControl, synth);
	/* Create the audio driver. */
	adriver = new_fluid_audio_driver2(settings, fx_func, (void *) &fx_data);
	
	seq = new_fluid_sequencer();
	synth_destination = fluid_sequencer_register_fluidsynth(seq, synth);
	client_destination = fluid_sequencer_register_client(seq, "test", seq_callback, NULL);
	
	/* Load a SoundFont and reset presets */
	sfont_id = fluid_synth_sfload(synth, "./samples/touhou.sf2", 1);
	if (sfont_id == FLUID_FAILED) {
		fprintf(stderr, "error on opening soundfont\n");
		goto cleanUp;
	}
	sfont = fluid_synth_get_sfont_by_id(synth, sfont_id);

	dplayer = new_fluid_player(synth);
	fluid_player_add(dplayer, "./drumloops/Downtempo.mid");
	fluid_player_set_loop(dplayer, -1);
	
	showInst(instrument);

	for (;;) {
		drumControl();

		if (!digitalRead(25)) distortion = 1;
		else distortion = 0;
		if (!digitalRead(0)) { // recording
			if (recording == 0) {
				total = 0;
				recording = 1;
				printf("recording on\n");
			}
		}
		else {
			if (recording == 1) {
				for (int i = 1; i < total; ++i) {
					sequence[i].time -= sequence[0].time;
				}
				sequence[0].time = 0;
				recording = 0;
				printf("recording off\n");
			}
		}

		if (!digitalRead(3)) { // recording playback
			fluid_event_t *ev = new_fluid_event();
			fluid_event_set_source(ev, -1);
			fluid_event_set_dest(ev, client_destination);
			fluid_event_timer(ev, NULL);
			fluid_sequencer_send_now(seq, ev);
			delete_fluid_event(ev);
			delay(100);
			// printf("p?\n");
		}
		
		delay(100);
	}
	
cleanUp:
	delete_fluid_audio_driver(adriver);
	delete_fluid_synth(synth);
	delete_fluid_settings(settings);
	return 0;
}

