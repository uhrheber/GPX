#include <Python.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <termios.h>
#else
#include "winsio.h"
#endif

#define USE_GPX_SIO_OPEN
#include "gpx.h"

// TODO at the moment the module is taking twice as much memory for Gpx as it
// should to because gpx-main also has one. We can merge them via extern or pass
// it one way or the other, but perhaps the best way is to drop gpx-main from
// the module and reserve that for the CLI.  We'll need to refactor a couple of
// things from it however
static Gpx gpx;

#define SHOW(FN) if(gpx->flag.logMessages) {FN;}
#define VERBOSE(FN) if(gpx->flag.verboseMode && gpx->flag.logMessages) {FN;}

// Sttb - string table
// need one of these to hang onto the filename list so that we can emulate
// case insensitivty since some host software expects that of gcode M23
typedef struct tSttb
{
    char **rgs;         // array of pointers
    size_t cb;          // count of bytes - allocated size of rgs
    size_t cb_expand;   // count of bytes to expand by each expansion
    long cs;            // count of strings currently stored Assert(cs * sizeof(char *) <= cb)
} Sttb;

// make a new string table
// cs_chunk -- count of strings -- grow the string array in chunks of this many strings
Sttb *sttb_init(Sttb *psttb, long cs_chunk)
{
    size_t cb = (size_t)cs_chunk * sizeof(char *);

    psttb->cs = 0;
    psttb->rgs = malloc(cb);
    if (psttb->rgs == NULL)
        return NULL;

    psttb->cb_expand = psttb->cb = cb;
    return psttb;
}

void sttb_cleanup(Sttb *psttb)
{
    char **ps = NULL;

    if (psttb->rgs == NULL)
        return;

    for (ps = psttb->rgs; ps < psttb->rgs + psttb->cs; ps++)
        free(*ps);

    free(psttb->rgs);
    psttb->rgs = NULL;
    psttb->cb = psttb->cb_expand = 0;
    psttb->cs = 0;
}

char *sttb_add(Sttb *psttb, char *s)
{
    if (psttb->rgs == NULL)
        return NULL;
    size_t cb_needed = sizeof(char *) * ((size_t)psttb->cs + 1);
    if (cb_needed > psttb->cb) {
        if (psttb->cb + psttb->cb_expand > cb_needed)
            cb_needed = psttb->cb + psttb->cb_expand;
        char **rgs_new = realloc(psttb->rgs, cb_needed);
        if (rgs_new == NULL)
            return NULL;
        psttb->rgs = rgs_new;
        psttb->cb = cb_needed;
    }
    if ((s = strdup(s)) == NULL)
        return NULL;
    return psttb->rgs[psttb->cs++] = s;
}

void sttb_remove(Sttb *psttb, long i)
{
    if (i < 0 || i >= psttb->cs) // a little bounds checking
        return;
    free(psttb->rgs[i]);
    memcpy(psttb->rgs + i, psttb->rgs + i + 1, (psttb->cs - i - 1) * sizeof(char *));
    psttb->cs--;
}

long sttb_find_nocase(Sttb *psttb, char *s)
{
    long i;
    for (i = 0; i < psttb->cs; i++) {
        if (strcasecmp(s, psttb->rgs[i]) == 0)
            return i;
    }
    return -1;
}


// Tio - translated serial io
// wraps Sio and adds translation output buffer
// translation is reprap style response
typedef struct tTio
{
    char translation[BUFFER_MAX + 1];
    size_t cur;
    Sio sio;
    struct {
        unsigned listingFiles:1;      // in the middle of an M20 response
    } flag;
    union {
        unsigned waiting;
        struct {
            unsigned waitForPlatform:1;
            unsigned waitForExtruderA:1;
            unsigned waitForExtruderB:1;
            unsigned waitForButton:1;
            unsigned waitForStart:1;
        } waitflag;
    };
    Sttb sttb;
    time_t sec;
} Tio;
static Tio tio;
static int connected = 0;

// Some custom python exceptions
static PyObject *pyerrBufferOverflow;

static int tio_vprintf(Tio *tio, const char *fmt, va_list ap)
{
    size_t result;

    if (tio->cur >= sizeof(tio->translation))
        return 0;
    result = vsnprintf(tio->translation + tio->cur,
            sizeof(tio->translation) - tio->cur, fmt, ap);
    if (result > 0)
        tio->cur += result;
    return result;
}

static int tio_printf(Tio *tio, char const* fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    result = tio_vprintf(tio, fmt, args);
    va_end(args);

    return result;
}

// clean up anything left over from before
static void gpx_cleanup(void)
{
    connected = 0;
    if (gpx.log != NULL && gpx.log != stderr) {
        fflush(gpx.log);
        fclose(gpx.log);
        gpx.log = stderr;
    }
    if (tio.sio.port > -1) {
        close(tio.sio.port);
        tio.sio.port = -1;
    }
    if (tio.sttb.cs > 0) {
        sttb_cleanup(&tio.sttb);
        sttb_init(&tio.sttb, 10);
    }
    tio.sec = 0;
}

// wrap port_handler and translate to the expect gcode response
#define COMMAND_OFFSET 2
#define EXTRUDER_ID_OFFSET 3
#define QUERY_COMMAND_OFFSET 4
#define EEPROM_LENGTH_OFFSET 8

static void translate_extruder_query_response(Gpx *gpx, Tio *tio, unsigned query_command, char *buffer)
{
    unsigned extruder_id = buffer[EXTRUDER_ID_OFFSET];

    switch (query_command) {
            // Query 00 - Query firmware version information
        case 0:
            // sio->response.firmware.version = read_16(gpx);
            break;

            // Query 02 - Get extruder temperature
        case 2:
            // like T0:170
            tio_printf(tio, " T");
            if (gpx->machine.extruder_count > 1)
                tio_printf(tio, "%u", extruder_id);
            tio_printf(tio, ":%u", tio->sio.response.temperature);
            break;

            // Query 22 - Is extruder ready
        case 22:
            if (extruder_id)
                tio->waitflag.waitForExtruderB = !tio->sio.response.isReady;
            else
                tio->waitflag.waitForExtruderA = !tio->sio.response.isReady;
            break;

            // Query 30 - Get build platform temperature
        case 30:
            tio_printf(tio, " B:%u", tio->sio.response.temperature);
            break;

            // Query 32 - Get extruder target temperature
        case 32:
            if (tio->waiting && tio->sio.response.temperature == 0) {
                if (extruder_id)
                    tio->waitflag.waitForExtruderB = 0;
                else
                    tio->waitflag.waitForExtruderA = 0;
            }
            // fallthrough
            //
            // Query 33 - Get build platform target temperature
        case 33:
            tio_printf(tio, " /%u", tio->sio.response.temperature);
            break;

            // Query 35 - Is build platform ready?
        case 35:
            tio->waitflag.waitForPlatform = !tio->sio.response.isReady;
            break;

            // Query 36 - Get extruder status
        case 36: /* NYI
            if(gpx->flag.verboseMode && gpx->flag.logMessages) {
                fprintf(gpx->log, "Extruder T%u status" EOL, extruder_id);
                if(sio->response.extruder.flag.ready) fputs("Target temperature reached" EOL, gpx->log);
                if(sio->response.extruder.flag.notPluggedIn) fputs("The extruder or build plate is not plugged in" EOL, gpx->log);
                if(sio->response.extruder.flag.softwareCutoff) fputs("Above maximum allowable temperature recorded: heater shutdown for safety" EOL, gpx->log);
                if(sio->response.extruder.flag.temperatureDropping) fputs("Heater temperature dropped below target temperature" EOL, gpx->log);
                if(sio->response.extruder.flag.buildPlateError) fputs("An error was detected with the build plate heater or sensor" EOL, gpx->log);
                if(sio->response.extruder.flag.extruderError) fputs("An error was detected with the extruder heater or sensor" EOL, gpx->log);
            } */
            break;

            // Query 37 - Get PID state
        case 37: /* NYI
            sio->response.pid.extruderError = read_16(gpx);
            sio->response.pid.extruderDelta = read_16(gpx);
            sio->response.pid.extruderOutput = read_16(gpx);
            sio->response.pid.buildPlateError = read_16(gpx);
            sio->response.pid.buildPlateDelta = read_16(gpx);
            sio->response.pid.buildPlateOutput = read_16(gpx); */
            break;
    }
}

// translate_handler
// Callback function for gpx_convert_and_send.  It's where we translate the
// s3g/x3g response into a text response that mimics a reprap printer.
static int translate_handler(Gpx *gpx, Tio *tio, char *buffer, size_t length)
{
    unsigned command;
    unsigned extruder;

    if (length == 0) {
        // we translated a command that has no translation to x3g, but there
        // still may be something to do to emulate gcode behavior
        if (gpx->command.flag & M_IS_SET) {
            switch (gpx->command.m) {
                case 23: { // M23 - select SD file
                    // Some host software expects case insensitivity for M23
                    long i = sttb_find_nocase(&tio->sttb, gpx->selectedFilename);
                    if (i >= 0) {
                        char *s = strdup(tio->sttb.rgs[i]);
                        if (s != NULL) {
                            free(gpx->selectedFilename);
                            gpx->selectedFilename = s;
                        }
                    }
                    // answer to M23, at least on Marlin, Repetier and Sprinter: "File opened:%s Size:%d"
                    // followed by "File selected:%s Size:%d".  Caller is going to 
                    // be really surprised when the failure happens on start print
                    // but other than enumerating all the files again, we don't
                    // have a way to tell the printer to go check if it can be
                    // opened
                    tio_printf(tio, " File opened:%s Size:%d\nok File selected:%s", gpx->selectedFilename, 0, gpx->selectedFilename);
                    // currently no way to ask Sailfish for the file size, that I can tell :-(
                    break;
                }
            }
        }
        return SUCCESS;
    }

    command = buffer[COMMAND_OFFSET];
    extruder = buffer[EXTRUDER_ID_OFFSET];
    int rval = port_handler(gpx, &tio->sio, buffer, length);
    if (rval != SUCCESS) {
        VERBOSE(fprintf(gpx->log, "port_handler returned: rval = %d\n", rval);)
        return rval;
    }

    switch (command) {
            // 03 - Clear buffer
        case 3:
            // 07 - Abort immediately
        case 7:
            // 17 - reset
        case 17:
            tio->waiting = 0;
            break;

            // 10 - Extruder (tool) query response
        case 10: {
            unsigned query_command = buffer[QUERY_COMMAND_OFFSET];
            translate_extruder_query_response(gpx, tio, query_command, buffer);
            break;
        }

            // 11 - Is ready?
        case 11:
            tio->waitflag.waitForButton = !tio->sio.response.isReady;
            break;

            // 14 - Begin capture to file
        case 14:
            if (gpx->command.flag & ARG_IS_SET)
                tio_printf(tio, "\nWriting to file: %s", gpx->command.arg);
            break;

            // 15 - End capture
        case 15:
            tio_printf(tio, "\nDone saving file");
            break;

            // 16 - Playback capture (print from SD)
        case 16:
            // give the bot a chance to clear the BUILD_CANCELLED from the previous build
            if (tio->sio.response.sd.status == 7)
                tio_printf(tio, "\nError:  Not SD printing file not found");
            else {
                tio->cur = 0;
                tio->translation[0] = 0;
                tio->sec = time(NULL) + 3;
                tio->waitflag.waitForStart = 1;
            }
            break;

            // 18 - Get next filename
        case 18:
            if (!tio->flag.listingFiles && (gpx->command.flag & M_IS_SET) && gpx->command.m == 21) {
                // we used "get_next_filename(1)" to emulate M21
                if (tio->sio.response.sd.status == 0)
                    tio_printf(tio, "\nSD card ok");
                else
                    tio_printf(tio, "\nSD init fail");
            }
            else {
                // otherwise generate the M20 response
                if (!tio->flag.listingFiles) {
                    tio_printf(tio, "\nBegin file list\n");
                    tio->flag.listingFiles = 1;
                    if (tio->sttb.cs > 0)
                        sttb_cleanup(&tio->sttb);
                    sttb_init(&tio->sttb, 10);
                }
                if (!tio->sio.response.sd.filename[0]) {
                    tio_printf(tio, "End file list");
                    tio->flag.listingFiles = 0;
                }
                else {
                    tio_printf(tio, "%s", tio->sio.response.sd.filename);
                    sttb_add(&tio->sttb, tio->sio.response.sd.filename);
                }
            }
            break;

            // 21 - Get extended position
        case 21: {
            double epos = (double)tio->sio.response.position.a / gpx->machine.a.steps_per_mm;
            if (gpx->current.extruder == 1)
                epos = (double)tio->sio.response.position.b / gpx->machine.b.steps_per_mm;
            tio_printf(tio, " X:%0.2f Y:%0.2f Z:%0.2f E:%0.2f",
                (double)tio->sio.response.position.x / gpx->machine.x.steps_per_mm,
                (double)tio->sio.response.position.y / gpx->machine.y.steps_per_mm,
                (double)tio->sio.response.position.z / gpx->machine.z.steps_per_mm,
                epos);
            }
            break;

            // 23 - Get motherboard status
        case 23:
            if (!tio->sio.response.motherboard.bitfield)
                tio->waitflag.waitForButton = 0;
            else {
                if (tio->sio.response.motherboard.flag.buildCancelling)
                    return 0x89;
                if (tio->sio.response.motherboard.flag.heatShutdown) {
                    tio->cur = 0;
                    tio_printf(tio, "Error:  Heaters were shutdown after 30 minutes of inactivity");
                    return 0x89;
                }
                if (tio->sio.response.motherboard.flag.powerError) {
                    tio->cur = 0;
                    tio_printf(tio, "Error:  Error detected in system power");
                    return 0x89;
                }
            }
            break;

            // Query 24 - Get build statistics
        case 24: {
            time_t t;
            if (tio->sec && tio->sio.response.build.status != 1 && time(&t) < tio->sec) {
                if ((tio->sec - t) > 4) {
                    // in case we have a clock discontinuity we don't want to ignore status forever
                    tio->sec = 0;
                    tio->waitflag.waitForStart = 0;
                }
                break; // ignore it if we haven't started yet
            }
            switch (tio->sio.response.build.status) {
                case 0:
                    // this is a matter of Not SD printing *yet* when we just
                    // kicked off the print, let's give it a moment
                    tio_printf(tio, " Not SD printing");
                    break;
                case 1:
                    tio->sec = 0;
                    tio->waitflag.waitForStart = 0;
                    tio_printf(tio, " SD printing byte on line %u/0", tio->sio.response.build.lineNumber);
                    break;
                case 4:
                    tio_printf(tio, " SD printing cancelled. ");
                    tio->waiting = 0;
                    // fall through
                case 2:
                    tio_printf(tio, " Done printing file");
                    break;
                case 3:
                    tio_printf(tio, " SD printing paused at line %u", tio->sio.response.build.lineNumber);
                    break;
                case 5:
                    tio_printf(tio, " SD printing sleeping at line %u", tio->sio.response.build.lineNumber);
                    break;
            }
            break;
        }

            // 27 - Get advanced version number
        case 27: {
            char *variant = "Unknown";
            switch(tio->sio.response.firmware.variant) {
                case 0x01:
                    variant = "Makerbot";
                    break;
                case 0x80:
                    variant = "Sailfish";
                    break;
            }
            tio_printf(tio, "%s v%u.%u", variant, tio->sio.response.firmware.version / 100, tio->sio.response.firmware.version % 100);
            break;

            // 135 - wait for extruder
        case 135:
            tio->cur = 0;
            tio->translation[0] = 0;
            VERBOSE( fprintf(gpx->log, "waiting for extruder %d\n", extruder) );
            if (extruder == 0)
                tio->waitflag.waitForExtruderA = 1;
            else
                tio->waitflag.waitForExtruderB = 1;
            break;

            // 141 - wait for build platform
        case 141:
            tio->cur = 0;
            tio->translation[0] = 0;
            VERBOSE( fprintf(gpx->log, "waiting for platform\n") );
            tio->waitflag.waitForPlatform = 1;
            break;

            // 148, 149 - message to the LCD, may be waiting for a button
        case 148:
        case 149:
            tio->cur = 0;
            tio->translation[0] = 0;
            VERBOSE( fprintf(gpx->log, "waiting for button\n") );
            tio->waitflag.waitForButton = 1;
            break;
        }
    }

    return rval;
}

static int translate_result(Gpx *gpx, Tio *tio, const char *fmt, va_list ap)
{
    int rval = 0;
    if (tio->cur == 2)
       rval = tio_printf(tio, "\n"); 
    return rval + tio_printf(tio, "// echo: ") + tio_vprintf(tio, fmt, ap);
}

// return the translation or set the error context and return NULL if failure
static PyObject *gpx_return_translation(int rval)
{
    // ENDED -> READY
    if (gpx.flag.programState > 1)
        gpx.flag.programState = 0;
    gpx.flag.macrosEnabled = 1;

    // if we're waiting for something and we haven't produced any output
    // give back current temps
    if (rval == SUCCESS && tio.waiting && tio.cur == 0) {
        strncpy(gpx.buffer.in, "M105", sizeof(gpx.buffer.in));
        rval = gpx_convert_line(&gpx, gpx.buffer.in);
    }

    fflush(gpx.log);
    switch (rval) {
        case SUCCESS:
            break;

        case EOSERROR:
            return PyErr_SetFromErrno(PyExc_IOError);
        case ERROR:
            PyErr_SetString(PyExc_IOError, "GPX error");
            return NULL;
        case ESIOWRITE:
        case ESIOREAD:
        case ESIOFRAME:
        case ESIOCRC:
            PyErr_SetString(PyExc_IOError, "Serial communication error");
            return NULL;
        case 0x80:
            PyErr_SetString(PyExc_IOError, "Generic Packet error");
            return NULL;
        case 0x82: // Action buffer overflow
            PyErr_SetString(pyerrBufferOverflow, "Buffer overflow");
            return NULL;
        case 0x83:
            tio.cur = 0;
            tio_printf(&tio, "Error: checksum mismatch");
            break;
        case 0x84:
            PyErr_SetString(PyExc_IOError, "Query packet too big");
            return NULL;
        case 0x85:
            PyErr_SetString(PyExc_IOError, "Command not supported or recognized");
            return NULL;
        case 0x87:
            tio.cur = 0;
            tio_printf(&tio, "Error: timeout downstream");
            break;
        case 0x88:
            tio.cur = 0;
            tio_printf(&tio, "Error: timeout for tool lock");
            break;
        case 0x89:
            tio.cur = 0;
            tio_printf(&tio, "Error: Cancel build");
            tio.waiting = 0;
            break;
        case 0x8A:
            tio.cur = 0;
            tio_printf(&tio, "SD printing");
            break;
        case 0x8B:
            tio.cur = 0;
            tio_printf(&tio, "Error: Shutdown due to MAXTEMP trigger");
            break;
        case 0x8C:
            tio.cur = 0;
            tio_printf(&tio, "Error: timeout");
            break;

        default:
            if (gpx.flag.verboseMode)
                fprintf(gpx.log, "Unknown error code: %d", rval);
            PyErr_SetString(PyExc_IOError, "Unknown error.");
            return NULL;
    }

    fflush(gpx.log);
    return Py_BuildValue("s", tio.translation);
}


static PyObject *gpx_write_string(const char *s)
{
    unsigned waiting = tio.waiting;

    strncpy(gpx.buffer.in, s, sizeof(gpx.buffer.in));
    int rval = gpx_convert_line(&gpx, gpx.buffer.in);

    // if we were waiting, but now we're not, throw an ok on there
    if (!tio.waiting && waiting)
        tio_printf(&tio, "\nok");

    return gpx_return_translation(rval);
}

// convert from a long int value to a speed_t constant
// on error, sets python error state and returns B0
speed_t speed_from_long(long *baudrate)
{
    speed_t speed = B0;

    // TODO Baudrate warning for Replicator 2/2X with 57600?  Throw maybe?
    switch(*baudrate) {
        case 4800:
            speed=B4800;
            break;
        case 9600:
            speed=B9600;
            break;
#ifdef B14400
        case 14400:
            speed=B14400;
            break;
#endif
        case 19200:
            speed=B19200;
            break;
#ifdef B28800
        case 28800:
            speed=B28800;
            break;
#endif
        case 38400:
            speed=B38400;
            break;
        case 57600:
            speed=B57600;
            break;
            // TODO auto detect speed when 0?
        case 0: // 0 means default of 115200
            *baudrate=115200;
        case 115200:
            speed=B115200;
            break;
        default:
            sprintf(gpx.buffer.out, "Unsupported baud rate '%ld'\n", *baudrate);
            fprintf(gpx.log, gpx.buffer.out);
            PyErr_SetString(PyExc_ValueError, gpx.buffer.out);
            break;
    }
    return speed;
}

// def connect(port, baudrate, inipath, logpath)
static PyObject *gpx_connect(PyObject *self, PyObject *args)
{
    const char *port = NULL;
    long baudrate = 0;
    const char *inipath = NULL;
    const char *logpath = NULL;
    int verbose = 0;

    if (!PyArg_ParseTuple(args, "s|lssi", &port, &baudrate, &inipath, &logpath, &verbose))
        return NULL;

    gpx_cleanup();
    gpx_initialize(&gpx, 0);

    // open the log file
    if (logpath != NULL && (gpx.log = fopen(logpath, "a")) == NULL) {
        fprintf(stderr, "Unable to open logfile (%s) for writing\n", logpath);
    }
    if (gpx.log == NULL)
        gpx.log = stderr;
#ifdef ALWAYS_USE_STDERR
    else if (gpx.log != stderr)
    {
        fclose(gpx.log);
        gpx.log = stderr;
    }
#endif

    gpx.flag.verboseMode = verbose;
    gpx.flag.logMessages = 1;

    // load the config
    if (inipath != NULL)
    {
        int lineno = gpx_load_config(&gpx, inipath);
        if (lineno < 0)
            fprintf(gpx.log, "Unable to load configuration file (%s)\n", inipath);
        if (lineno > 0)
            fprintf(gpx.log, "(line %u) Configuration syntax error in %s: unrecognized parameters\n", lineno, inipath);
    }

    // open the port
    speed_t speed = speed_from_long(&baudrate);
    if (speed == B0)
        return NULL;
    if (!gpx_sio_open(&gpx, port, speed, &tio.sio.port)) {
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, port);
    }

    // TODO build_name and framing
    gpx_start_convert(&gpx, "", 0);

    gpx.flag.framingEnabled = 1;
    gpx.flag.sioConnected = 1;
    gpx_register_callback(&gpx, (int (*)(Gpx*, void*, char*, size_t))translate_handler, &tio);
    gpx.resultHandler = (int (*)(Gpx*, void*, const char*, va_list))translate_result;

    tio.sio.in = NULL;
    tio.sio.bytes_out = tio.sio.bytes_in = 0;
    tio.sio.flag.retryBufferOverflow = 1;
    connected = 1;

    fprintf(gpx.log, "gpx connected to %s at %ld using %s and %s\n", port, baudrate, inipath, logpath);

    tio.cur = 0;
    tio_printf(&tio, "start\n");
    int rval = get_advanced_version_number(&gpx);
    if (rval >= 0) {
        tio_printf(&tio, "echo: gcode to x3g translation by GPX");
        return gpx_write_string("M21");
    }
    return gpx_return_translation(rval);
}

static PyObject *PyErr_NotConnected(void)
{
    PyErr_SetString(PyExc_IOError, "Not connected");
    return NULL;
}

// def write(data)
static PyObject *gpx_write(PyObject *self, PyObject *args)
{
    char *line;

    if (!connected)
        return PyErr_NotConnected();

    if (!PyArg_ParseTuple(args, "s", &line))
        return NULL;

    tio.cur = 0;
    tio.translation[0] = 0;
    if (!tio.waiting)
        tio_printf(&tio, "ok"); // ok message received, so error message needs to be \nerror

    return gpx_write_string(line);
}

// def readnext()
static PyObject *gpx_readnext(PyObject *self, PyObject *args)
{
    int rval = SUCCESS;

    if (!connected)
        return PyErr_NotConnected();

    if (!PyArg_ParseTuple(args, ""))
        return NULL;

    tio.cur = 0;
    tio.translation[0] = 0;

    if (tio.flag.listingFiles) {
        rval = get_next_filename(&gpx, 0);
    }
    else if (tio.waiting) {
        if (gpx.flag.verboseMode && gpx.flag.logMessages)
            fprintf(gpx.log, "tio.waiting = %u\n", tio.waiting);
        if (tio.waitflag.waitForStart)
            rval = get_build_statistics(&gpx);
        if (rval == SUCCESS && tio.waitflag.waitForPlatform)
            rval = is_build_platform_ready(&gpx, 0);
        if (rval == SUCCESS && tio.waitflag.waitForExtruderA)
            rval = is_extruder_ready(&gpx, 0);
        if (rval == SUCCESS && tio.waitflag.waitForExtruderB)
            rval = is_extruder_ready(&gpx, 1);
        if (rval == SUCCESS && tio.waitflag.waitForButton)
            rval = is_ready(&gpx);
        if (gpx.flag.verboseMode && gpx.flag.logMessages)
            fprintf(gpx.log, "tio.waiting = %u and rval = %d\n", tio.waiting, rval);
        if (rval == SUCCESS) {
            if (tio.waiting) {
                return gpx_write_string("M105");
            }
            tio.cur = 0;
            tio_printf(&tio, "ok");
        }
    }
    return gpx_return_translation(rval);
}

#if !defined(_WIN32) && !defined(_WIN64)
// def baudrate(long)
static PyObject *gpx_set_baudrate(PyObject *self, PyObject *args)
{
    struct termios tp;
    long baudrate;
    speed_t speed;

    if (!connected)
        return PyErr_NotConnected();

    if (!PyArg_ParseTuple(args, "l", &baudrate))
        return NULL;

    if(tcgetattr(tio.sio.port, &tp) < 0)
        return PyErr_SetFromErrno(PyExc_IOError);
    speed = speed_from_long(&baudrate);
    if (speed == B0)
        return NULL;
    cfsetspeed(&tp, speed);
    if(tcsetattr(tio.sio.port, TCSANOW, &tp) < 0)
        return PyErr_SetFromErrno(PyExc_IOError);

    return Py_BuildValue("i", 0);
}
#else
static PyObject *gpx_set_baudrate(PyObject *self, PyObject *args)
{
    long baudrate;

    if (!connected)
        return PyErr_NotConnected();

    if (!PyArg_ParseTuple(args, "l", &baudrate))
        return NULL;

    // TODO Windows set_baudrate

    return NULL;
}
#endif // _WIN32 || _WIN64

// def disconnect()
static PyObject *gpx_disconnect(PyObject *self, PyObject *args)
{
    gpx_cleanup();
    if (!PyArg_ParseTuple(args, ""))
        return NULL;
    return Py_BuildValue("i", 0);
}

// def get_machine_defaults(machine_type_id)
// for example: machine_info = get_machine_defaults("r1d")
static PyObject *gpx_get_machine_defaults(PyObject *self, PyObject *args)
{
    char *machine_type_id;

    if (!PyArg_ParseTuple(args, "s", &machine_type_id))
        return NULL;

    Machine *machine = gpx_find_machine(machine_type_id);
    fflush(gpx.log);
    if (machine == NULL) {
        PyErr_SetString(PyExc_ValueError, "Machine id not found");
        return NULL;
    }

    return Py_BuildValue("{s{sd,sd,sd,si}s{sd,sd,sd,si}s{sd,sd,sd,si}s{sd,sd,sd,si}s{sd,sd,sd,si}s{sdsdsdsisi}}",
        "x",
            "max_feedrate", machine->x.max_feedrate,
            "home_feedrate", machine->x.home_feedrate,
            "steps_per_mm", machine->x.steps_per_mm,
            "endstop", machine->x.endstop,
        "y",
            "max_feedrate", machine->y.max_feedrate,
            "home_feedrate", machine->y.home_feedrate,
            "steps_per_mm", machine->y.steps_per_mm,
            "endstop", machine->y.endstop,
        "z",
            "max_feedrate", machine->z.max_feedrate,
            "home_feedrate", machine->z.home_feedrate,
            "steps_per_mm", machine->z.steps_per_mm,
            "endstop", machine->z.endstop,
        "a",
            "max_feedrate", machine->a.max_feedrate,
            "steps_per_mm", machine->a.steps_per_mm,
            "motor_steps", machine->a.motor_steps,
            "has_heated_build_platform", machine->a.has_heated_build_platform,
        "b",
            "max_feedrate", machine->b.max_feedrate,
            "steps_per_mm", machine->b.steps_per_mm,
            "motor_steps", machine->b.motor_steps,
            "has_heated_build_platform", machine->b.has_heated_build_platform,
        "machine",
            "slicer_filament_diameter", machine->nominal_filament_diameter,
            "packing_density", machine->nominal_packing_density,
            "nozzle_diameter", machine->nozzle_diameter,
            "extruder_count", machine->extruder_count,
            "timeout", machine->timeout
    );

}

// def read_ini(ini_filepath)
static PyObject *gpx_read_ini(PyObject *self, PyObject *args)
{
    const char *inipath = NULL;

    if (!PyArg_ParseTuple(args, "s", &inipath))
        return NULL;

    int lineno = gpx_load_config(&gpx, inipath);
    if (lineno == 0)
        return Py_BuildValue("i", 0); // success

    if (lineno < 0)
        fprintf(gpx.log, "Unable to load configuration file (%s)\n", inipath);
    if (lineno > 0)
        fprintf(gpx.log, "(line %u) Configuration syntax error in %s: unrecognized parameters\n", lineno, inipath);
    fflush(gpx.log);

    PyErr_SetString(PyExc_ValueError, "Unable to load ini file");
    return Py_BuildValue("i", 0);
}

// def reset_ini()
// reset settings to defaults
static PyObject *gpx_reset_ini(PyObject *self, PyObject *args)
{
    // some state survives reset_ini
    void *callbackHandler = gpx.callbackHandler;
    void *resultHandler = gpx.resultHandler;
    void *callbackData = gpx.callbackData;
    FILE *log = gpx.log;
    unsigned verbose = gpx.flag.verboseMode;

    // nuke it all
    gpx_initialize(&gpx, 1);

    // restore some stuff, plus we're still in pymodule mode
    gpx.callbackHandler = callbackHandler;
    gpx.resultHandler = resultHandler;
    gpx.callbackData = callbackData;
    gpx.log = log;
    gpx.flag.framingEnabled = 1;
    gpx.flag.sioConnected = 1;
    gpx.flag.verboseMode = verbose;
    gpx.flag.logMessages = 1;

    return Py_BuildValue("i", 0);
}

// def waiting()
// is the bot waiting for something?
static PyObject *gpx_waiting(PyObject *self, PyObject *args)
{
    if (!connected)
        return PyErr_NotConnected();

    if (tio.waiting)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

// def reprap_flavor(turn_on_reprap)
static PyObject *gpx_reprap_flavor(PyObject *self, PyObject *args)
{
    if (!connected)
        return PyErr_NotConnected();

    int reprap = 1;
    if (!PyArg_ParseTuple(args, "i", &reprap))
        return NULL;

    int rval = gpx.flag.reprapFlavor;
    gpx.flag.reprapFlavor = !!reprap;
    if (rval)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}


// method table describes what is exposed to python
static PyMethodDef GpxMethods[] = {
    {"connect", gpx_connect, METH_VARARGS, "connect(port, baud = 0, inifilepath = None, logfilepath = None) Open the serial port to the printer and initialize the channel"},
    {"disconnect", gpx_disconnect, METH_VARARGS, "disconnect() Close the serial port and clean up."},
    {"write", gpx_write, METH_VARARGS, "write(string) Translate g-code into x3g and send."},
    {"readnext", gpx_readnext, METH_VARARGS, "readnext() read next response if any"},
    {"set_baudrate", gpx_set_baudrate, METH_VARARGS, "set_baudrate(long) Set the current baudrate for the connection to the printer."},
    {"get_machine_defaults", gpx_get_machine_defaults, METH_VARARGS, "get_machine_defaults(string) Return a dict with the default settings for the indicated machine type."},
    {"read_ini", gpx_read_ini, METH_VARARGS, "read_ini(string) Parse indicated ini file for gpx settings and macros and update current converter state. Loading ini files is additive. They just build on the ini's that have been read before. Use reset_ini to start from a clean state again."},
    {"reset_ini", gpx_reset_ini, METH_VARARGS, "reset_ini() Reset configuration state to default"},
    {"waiting", gpx_waiting, METH_VARARGS, "waiting() Returns True if the bot reports it is waiting for a temperature, pause or prompt"},
    {"reprap_flavor", gpx_reprap_flavor, METH_VARARGS, "reprap_flavor(boolean) Sets the expected gcode flavor (true = reprap, false = makerbot), returns the previous setting"},
    {NULL, NULL, 0, NULL} // sentinel
};

__attribute__ ((visibility ("default"))) PyMODINIT_FUNC initgpx(void);

// python calls init<modulename> when the module is loaded
PyMODINIT_FUNC initgpx(void)
{
    PyObject *m = Py_InitModule("gpx", GpxMethods);
    if (m == NULL)
        return;

    pyerrBufferOverflow = PyErr_NewException("gpx.BufferOverflow", NULL, NULL);
    Py_INCREF(pyerrBufferOverflow);
    PyModule_AddObject(m, "BufferOverflow", pyerrBufferOverflow);

    tio.sio.port = -1;
    tio.flag.listingFiles = 0;
    tio.waiting = 0;
    tio.sec = 0;
    sttb_init(&tio.sttb, 10);
    gpx_initialize(&gpx, 1);
    gpx.flag.M106AlwaysValve = 1;
}
