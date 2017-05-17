#include <Python.h>
#include <assert.h>

#include "../RpiTx.h"
#include "../RpiGpio.h"

#define COUNT_OF(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

struct module_state {
	PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))
#else
#define GETSTATE(m) (&_state)
static struct module_state _state;
#endif

static int streamFileNo = 0;
static int sampleRate = 48000;

typedef struct {
	double frequency;
	uint32_t waitForThisSample;
} samplerf_t;

static size_t read_float(int streamFileNo, float* wavBuffer, const size_t count);

/**
 * Formats a chunk of an array of a mono 48k wav at a time and outputs IQ
 * formatted array for broadcast.
 */
static ssize_t formatRfWrapper(void* const outBuffer, const size_t count) {
	static float wavBuffer[1024];
	static int wavOffset = -1;
	static int wavFilled = -1;

	if (wavFilled == 0) {
		return 0;
	}

	const int excursion = 6000;
	int numBytesWritten = 0;
	samplerf_t samplerf;
	samplerf.waitForThisSample = 1e9 / ((float)sampleRate);  //en 100 de nanosecond
	char* const out = outBuffer;

	while (numBytesWritten <= count - sizeof(samplerf_t)) {
		for (
				;
				numBytesWritten <= count - sizeof(samplerf_t) && wavOffset < wavFilled;
				++wavOffset
		) {
			const float x = wavBuffer[wavOffset];
			samplerf.frequency = x * excursion * 2.0;
			memcpy(&out[numBytesWritten], &samplerf, sizeof(samplerf_t));
			numBytesWritten += sizeof(samplerf_t);
		}

		assert(wavOffset <= wavFilled);

		if (wavOffset == wavFilled) {
			wavFilled = read_float(streamFileNo, wavBuffer, COUNT_OF(wavBuffer));
			wavOffset = 0;
		}
	}
	return numBytesWritten;
}
static size_t read_float(int streamFileNo, float* wavBuffer, const size_t count) {
	// Samples are stored as 16 bit signed integers in range of -32768 to 32767
	uint16_t sample;
	int i;
	for (i = 0; i < count; ++i) {
		const int byteCount = read(streamFileNo, &sample, sizeof(sample));
		if (byteCount != sizeof(sample)) {
			return i;
		}
		// TODO: I don't know if this should be dividing by 32767 or 32768. Probably
		// doesn't matter too much.
		*wavBuffer = ((float)le16toh(sample)) / 32768;
		++wavBuffer;
	}
	return count;
}
static void dummyFunction(void) {
	assert(0 && "dummyFunction should not be called");
}

#define RETURN_ERROR(errorMessage) \
PyErr_SetString(st->error, errorMessage); \
return NULL;

static PyObject*
_rpitx_broadcast_fm(PyObject* self, PyObject* args) {
	float frequency;

	struct module_state *st = GETSTATE(self);

	if (!PyArg_ParseTuple(args, "if", &streamFileNo, &frequency)) {
		RETURN_ERROR("Invalid arguments");
		return NULL;
	}

	union {
		char char4[4];
		uint32_t uint32;
		uint16_t uint16;
	} buffer;
	size_t byteCount = read(streamFileNo, buffer.char4, 4);
	if (byteCount != 4 || strncmp(buffer.char4, "RIFF", 4) != 0) {
		RETURN_ERROR("Not a WAV file");
	}

	// Skip chunk size
	byteCount = read(streamFileNo, buffer.char4, 4);
	if (byteCount != 4) {
		RETURN_ERROR("Not a WAV file");
	}

	byteCount = read(streamFileNo, buffer.char4, 4);
	if (byteCount != 4 || strncmp(buffer.char4, "WAVE", 4) != 0) {
		RETURN_ERROR("Not a WAV file");
	}

	byteCount = read(streamFileNo, buffer.char4, 4);
	if (byteCount != 4 || strncmp(buffer.char4, "fmt ", 4) != 0) {
		RETURN_ERROR("Not a WAV file");
	}

	byteCount = read(streamFileNo, &buffer.uint32, 4);
	buffer.uint32 = le32toh(buffer.uint32);
	// TODO: This value is coming out as 18 and I don't know why, so I'm
	// skipping this check for now
	/*
	if (byteCount != 4 || buffer.uint32 != 16) {
		printf("byteCount %d uint32 %d\n", byteCount, buffer.uint32);
		RETURN_ERROR("Not a PCM WAV file");
	}
	*/

	byteCount = read(streamFileNo, &buffer.uint16, 2);
	buffer.uint16 = le16toh(buffer.uint16);
	if (byteCount != 2 || buffer.uint16 != 1) {
		RETURN_ERROR("Not an uncompressed WAV file");
	}

	byteCount = read(streamFileNo, &buffer.uint16, 2);
	buffer.uint16 = le16toh(buffer.uint16);
	if (byteCount != 2 || buffer.uint16 != 1) {
		RETURN_ERROR("Not a mono WAV file");
	}

	byteCount = read(streamFileNo, &buffer.uint32, 4);
	sampleRate = le32toh(buffer.uint32);
	if (byteCount != 4 || sampleRate != 48000) {
		RETURN_ERROR("Not a WAV file");
	}

	// Skip byte rate
	byteCount = read(streamFileNo, &buffer.uint32, 4);
	if (byteCount != 4) {
		RETURN_ERROR("Not a WAV file");
	}

	// Skip block align
	byteCount = read(streamFileNo, &buffer.uint16, 2);
	if (byteCount != 2) {
		RETURN_ERROR("Not a WAV file");
	}

	byteCount = read(streamFileNo, &buffer.uint16, 2);
	buffer.uint16 = le16toh(buffer.uint16);
	if (byteCount != 2 || buffer.uint16 != 16) {
		RETURN_ERROR("Not a 16 bit WAV file");
	}

	byteCount = read(streamFileNo, buffer.char4, 4);
	// TODO: Normal WAV files have "data" here, but avconv spits out "LIST".
	// This might be because it's not spitting out a PCM file? It's putting
	// two blank '\0' and then "LIST".
	if (byteCount == 4
			&& buffer.char4[0] == '\0'
			&& buffer.char4[1] == '\0'
			&& buffer.char4[2] == 'L'
			&& buffer.char4[3] == 'I') {
		read(streamFileNo, buffer.char4, 2);
	} else
	if (byteCount != 4 || strncmp(buffer.char4, "data", 4) != 0) {
		printf("byteCount %d buffer.char4 %d %d %d %d\n", byteCount, buffer.char4[0], buffer.char4[1], buffer.char4[2], buffer.char4[3]);
		RETURN_ERROR("Not a WAV file");
	}

	// Skip subchunk2 size
	byteCount = read(streamFileNo, &buffer.uint32, 4);
	if (byteCount != 4) {
		RETURN_ERROR("Not a WAV file");
	}

	int skipSignals[] = {
		SIGALRM,
		SIGVTALRM,
		SIGCHLD,  // We fork whenever calling broadcast_fm
		SIGWINCH,  // Window resized
		0
	};

	pitx_run(MODE_RF, sampleRate, frequency * 1000.0, 0.0, 0, formatRfWrapper, dummyFunction, skipSignals, 0);

	Py_RETURN_NONE;
}


static PyMethodDef _rpitx_methods[] = {
	{
		"broadcast_fm",
		_rpitx_broadcast_fm,
		METH_VARARGS,
		"Low-level broadcasting.\n\n"
			"Broadcast a WAV formatted 48KHz file from a pipe file descriptor.\n"
			"Args:\n"
			"    pipe_file_no (int): The fileno of the pipe that the WAV is being written to.\n"
			"    frequency (float): The frequency, in MHz, to broadcast on.\n"
	},
	{NULL, NULL, 0, NULL}
};


#if PY_MAJOR_VERSION >= 3
static int _rpitx_traverse(PyObject* m, visitproc visit, void* arg) {
	Py_VISIT(GETSTATE(m)->error);
	return 0;
}


static int _rpitx_clear(PyObject *m) {
	Py_CLEAR(GETSTATE(m)->error);
	return 0;
}


static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"_rpitx",
	NULL,
	sizeof(struct module_state),
	_rpitx_methods,
	NULL,
	_rpitx_traverse,
	_rpitx_clear,
	NULL
};

#define INITERROR return NULL

PyObject*
PyInit__rpitx(void)

#else

#define INITERROR return

void
init_rpitx(void)
#endif
{
#if PY_MAJOR_VERSION >= 3
	PyObject* const module = PyModule_Create(&moduledef);
#else
	PyObject* const module = Py_InitModule("_rpitx", _rpitx_methods);
#endif
	if (module == NULL) {
		INITERROR;
	}
	struct module_state* st = GETSTATE(module);
	st->error = PyErr_NewException("_rpitx.Error", NULL, NULL);
	if (st->error == NULL) {
		Py_DECREF(module);
		INITERROR;
	}
	Py_INCREF(st->error);
	PyModule_AddObject(module, "error", st->error);

#if PY_MAJOR_VERSION >= 3
	return module;
#endif
}
