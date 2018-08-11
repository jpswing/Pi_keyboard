#include <fluidsynth.h>
#include <stdlib.h>
#include <wiringPi.h>
#include <math.h>

#define abs(x) ((x) < 0 ? -(x) : (x))

struct fx_data_t {
	fluid_synth_t *synth;
	float val;
};
int instrument = 0, octave = 0;
fluid_settings_t* settings = NULL;
fluid_synth_t* synth = NULL;
fluid_sfont_t* sfont = NULL;
fluid_audio_driver_t* adriver = NULL;
fluid_midi_driver_t* mdriver = NULL;
fluid_player_t* dplayer = NULL;
const int CMajor[7] = {60, 62, 64, 65, 67, 69, 71};

int distortion = 0;

void showInst(int preNum) {
	fluid_preset_t* ps = sfont->get_preset(sfont, 0, preNum);
	printf("current instrument: No.%d: %s\n", preNum, 
			ps->get_name(ps));
}

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

void octaveControl() {
	if (!digitalRead(7)) {
		++octave;
		if (octave > 1) octave = -1;
		delay(100);
	}
}

int playing[30] = {0};
void noteControl() {
	for (int i = 0; i <= 6; ++i) {
		int note = CMajor[i] + octave * 12;
		if (!digitalRead(i)) {
			if (!playing[i]) {
				fluid_synth_noteon(synth, 0, note, 80);
				playing[i] = 1;
			}
		}
		else {
			if (playing[i]) {
				fluid_synth_noteoff(synth, 0, note);
				playing[i] = 0;
			}
		}
	}
	for (int i = 21; i <= 24; ++i) {
		int note = CMajor[i - 21] + 12 + octave * 12;
		if (!digitalRead(i)) {
			if (!playing[i]) {
				fluid_synth_noteon(synth, 0, note, 80);
				playing[i] = 1;
			}
		}
		else {
			if (playing[i]) {
				fluid_synth_noteoff(synth, 0, note);
				playing[i] = 0;
			}
		}
	}
}

#define CHORD_NUM 2
const int CHORD[CHORD_NUM][12] = {
	{4, 7},
	{3, 7},
};
const char const *CHORD_NAME[CHORD_NUM] = {
	"Major",
	"Minor",
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
	for (int i = 1; i < cnt; ++i) {
		while (oct[i] - 12 >= oct[0]) oct[i] -= 12;
	}

	int itv[12];
	for (int i = 1; i < cnt; ++i) {
		itv[i - 1] = oct[i] - oct[0];
	}
	for (int chd = 0; chd < CHORD_NUM; ++chd) {
		int i;
		for (i = 0; i < cnt - 1; ++i) {
			if (itv[i] != CHORD[chd][i]) break;
		}
		if (i == cnt - 1) {
			printf("%s\n", CHORD_NAME[chd]);
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
	}
	else if (type == 128 && pitchs[pitch] == 1) {
		pitchs[pitch] = 0;
		--pcnt;
	}
	else if (type == 192) {
		instrument = fluid_midi_event_get_control(event);
		showInst(instrument);
	}
	// printf("%d %d %d\n", pcnt, type, pitch);
	return 0;
}

int looping = 0;
void drumControl() {
	if (!digitalRead(7)) {
		if (looping) {
			fluid_player_stop(dplayer);
			looping = 0;
		}
		else {
			fluid_player_play(dplayer);
			looping = 1;
		}
		delay(100);
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
	
	/* Load a SoundFont and reset presets */
	sfont_id = fluid_synth_sfload(synth, "./samples/touhou.sf2", 1);
	if (sfont_id == FLUID_FAILED) {
		fprintf(stderr, "error on opening soundfont\n");
		goto cleanUp;
	}
	sfont = fluid_synth_get_sfont_by_id(synth, sfont_id);

	dplayer = new_fluid_player(synth);
	fluid_player_add(dplayer, "./drumloops/Swing.mid");
	fluid_player_set_loop(dplayer, -1);
	
	printf("ready\n");
	showInst(instrument);

	for (;;) {
		noteControl();
		drumControl();

		if (!digitalRead(25)) distortion = 1;
		else distortion = 0;
		
		delay(100);
	}
	
cleanUp:
	delete_fluid_audio_driver(adriver);
	delete_fluid_synth(synth);
	delete_fluid_settings(settings);
	return 0;
}

