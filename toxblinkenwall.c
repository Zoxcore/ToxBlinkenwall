/*
 *
 * Zoff <zoff@zoff.cc>
 * in 2017
 *
 * compile on linux (dynamic):
 *  gcc -O2 -fPIC -Iusr/include -o toxblinkenwall toxblinkenwall.c -std=gnu99 -lsodium -I/usr/local/include/ \
        -Lusr/lib -ltoxcore -ltoxav -lpthread -lv4lconvert -lao -lasound
 *
 * run on linux (dynamic):
 * export LD_LIBRARY_PATH=usr/lib ; ./toxblinkenwall
 *
 * dirty hack (echobot and toxic were used as blueprint)
 *
 *
 *
 */

/*

# cat /proc/asound/cards

# aplay -l | awk -F \: '/,/{print $2}' | awk '{print $1}' | uniq


nano -w /usr/share/alsa/alsa.conf
# example usb audio: U0x4b40x301

add to end of file:

-----------------------------------------
pcm.usb
{
    type hw
    card U0x4b40x301
}

pcm.card_bcm {
    type hw
    card ALSA
}

pcm.!default {
    type asym

    playback.pcm
    {
        type plug
        slave.pcm "card_bcm"
    }

    capture.pcm
    {
        type plug
        slave.pcm "usb"
    }
}

defaults.pcm.!card ALSA
-----------------------------------------

# v4l2-ctl  --list-formats-ext
# v4l2-ctl -v width=1280,height=720,pixelformat=YV12
# v4l2-ctl --stream-mmap=3 --stream-count=50
#
# should be about 30fps

# search
dbg\([^.]*, "[^\\]*"



# sudo  lsusb -v | grep "idProduct\|MaxPower"


*/

#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

#include <pthread.h>

#include <semaphore.h>
#include <signal.h>
#include <linux/sched.h>

#include <sodium/utils.h>
#include <tox/tox.h>
#include <tox/toxav.h>

#include <linux/fb.h>
#include <linux/videodev2.h>
#include <vpx/vpx_image.h>
#include <sys/mman.h>

#define V4LCONVERT 1

// --------- video output: choose only 1 of those! ---------
#define HAVE_FRAMEBUFFER 1   // fb output           [* DEFAULT]
// --------- video output: choose only 1 of those! ---------
//
// --------- audio recording: choose only 1 of those! ---------
// #define HAVE_ALSA_REC 1      // for audio recording [* DEFAULT]
// --------- audio recording: choose only 1 of those! ---------
//
// --------- audio playing: choose only 1 of those! ---------
// #define HAVE_LIBAO 1      // for audio playing   (a bit choppy)
#define HAVE_ALSA_PLAY 1     // for audio playing   [* DEFAULT]
// #define HAVE_PORTAUDIO 1  // for audio playing   --> currently broken!!
// --------- audio playing: choose only 1 of those! ---------
//

// --------- external keys ---------
// #define HAVE_EXTERNAL_KEYS 1
// --------- external keys ---------


// ---------- dirty hack ----------
// ---------- dirty hack ----------
// ---------- dirty hack ----------
extern int global__MAX_DECODE_TIME_US;
extern int global__VP8E_SET_CPUUSED_VALUE;
extern int global__VPX_END_USAGE;
extern int global__VPX_KF_MAX_DIST;
extern int global__VPX_G_LAG_IN_FRAMES;
// ---------- dirty hack ----------
// ---------- dirty hack ----------
// ---------- dirty hack ----------


#if defined(HAVE_ALSA_REC) || defined(HAVE_ALSA_PLAY)
#include <alsa/asoundlib.h>
#endif

#ifdef HAVE_LIBAO
#include <ao/ao.h>
#endif

#ifdef HAVE_PORTAUDIO
#include <portaudio.h>
#include "ringbuf.h"
#endif


#ifdef V4LCONVERT
#include <libv4lconvert.h>
#endif

#define STBIR_SATURATE_INT 1
#define STBIR_DEFAULT_FILTER_UPSAMPLE STBIR_FILTER_BOX
#define STBIR_NO_ALPHA_EPSILON 1
#define STB_IMAGE_RESIZE_IMPLEMENTATION 1
#define STBIR_ASSERT(x) #x
#include "stb_image_resize.h"



#ifdef V4LCONVERT
static struct v4lconvert_data *v4lconvert_data;
#endif


// ----------- version -----------
// ----------- version -----------
#define VERSION_MAJOR 0
#define VERSION_MINOR 99
#define VERSION_PATCH 15
static const char global_version_string[] = "0.99.15";
// ----------- version -----------
// ----------- version -----------

typedef struct DHT_node {
    const char *ip;
    uint16_t port;
    const char key_hex[TOX_PUBLIC_KEY_SIZE*2 + 1];
    unsigned char key_bin[TOX_PUBLIC_KEY_SIZE];
} DHT_node;


struct audio_play_data_block {
    char *pcm; // memory block of PCM data
    size_t block_size_in_bytes;
	size_t sample_count;
};

struct alsa_audio_play_data_block {
    char *pcm; // memory block of PCM data
    size_t block_size_in_bytes;
	uint8_t channels;
	uint32_t sampling_rate;
	size_t sample_count;
};


#define MAX_AVATAR_FILE_SIZE 65536
#define TOXIC_MAX_NAME_LENGTH 32   /* Must be <= TOX_MAX_NAME_LENGTH */
#define TIME_STR_SIZE 32
#define MAX_STR_SIZE 200

#define CURRENT_LOG_LEVEL 9 // 0 -> error, 1 -> warn, 2 -> info, 9 -> debug

#define KiB 1024
#define MiB 1048576       /* 1024^2 */
#define GiB 1073741824    /* 1024^3 */

#define seconds_since_last_mod 1 // how long to wait before we process image files in seconds
#define MAX_FILES 6 // how many filetransfers to/from 1 friend at the same time?
#define MAX_RESEND_FILE_BEFORE_ASK 6
#define AUTO_RESEND_SECONDS 60*5 // resend for this much seconds before asking again [5 min]
#define VIDEO_BUFFER_COUNT 3 // 3 is ok --> HINT: more buffer will cause more video delay!
uint32_t DEFAULT_GLOBAL_VID_BITRATE = 2500; // kbit/sec
#define DEFAULT_GLOBAL_VID_BITRATE_NORMAL_QUALITY 2500 // kbit/sec
#define DEFAULT_GLOBAL_VID_BITRATE_HIGHER_QUALITY 10000 // kbit/sec
#define DEFAULT_GLOBAL_AUD_BITRATE 64 // kbit/sec
#define DEFAULT_GLOBAL_MIN_VID_BITRATE 300 // kbit/sec
#define DEFAULT_GLOBAL_MAX_VID_BITRATE 20000 // kbit/sec
#define DEFAULT_GLOBAL_MIN_AUD_BITRATE 6 // kbit/sec
int DEFAULT_FPS_SLEEP_MS = 110; // 250=4fps, 500=2fps, 160=6fps, 66=15fps, 40=25fps  // default video fps (sleep in msecs.)
int default_fps_sleep_corrected;

#define SWAP_R_AND_B_COLOR 1 // use BGRA instead of RGBA for raw framebuffer output

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define c_sleep(x) usleep(1000*x)

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })


#define CLIP(X) ( (X) > 255 ? 255 : (X) < 0 ? 0 : X)

// RGB -> YUV
#define RGB2Y(R, G, B) CLIP(( (  66 * (R) + 129 * (G) +  25 * (B) + 128) >> 8) +  16)
#define RGB2U(R, G, B) CLIP(( ( -38 * (R) -  74 * (G) + 112 * (B) + 128) >> 8) + 128)
#define RGB2V(R, G, B) CLIP(( ( 112 * (R) -  94 * (G) -  18 * (B) + 128) >> 8) + 128)

// YUV -> RGB
#define C(Y) ( (Y) - 16  )
#define D(U) ( (U) - 128 )
#define E(V) ( (V) - 128 )

#define YUV2R(Y, U, V) CLIP(( 298 * C(Y)              + 409 * E(V) + 128) >> 8)
#define YUV2G(Y, U, V) CLIP(( 298 * C(Y) - 100 * D(U) - 208 * E(V) + 128) >> 8)
#define YUV2B(Y, U, V) CLIP(( 298 * C(Y) + 516 * D(U)              + 128) >> 8)


typedef enum FILE_TRANSFER_STATE {
    FILE_TRANSFER_INACTIVE, // == 0 , this is important
    FILE_TRANSFER_PAUSED,
    FILE_TRANSFER_PENDING,
    FILE_TRANSFER_STARTED,
} FILE_TRANSFER_STATE;

typedef enum FILE_TRANSFER_DIRECTION {
    FILE_TRANSFER_SEND,
    FILE_TRANSFER_RECV
} FILE_TRANSFER_DIRECTION;

struct FileTransfer {
    FILE *file;
    FILE_TRANSFER_STATE state;
    FILE_TRANSFER_DIRECTION direction;
    uint8_t file_type;
    char file_name[TOX_MAX_FILENAME_LENGTH + 1];
    char file_path[PATH_MAX + 1];    /* Not used by senders */
    double   bps;
    uint32_t filenum;
    uint32_t friendnum;
    size_t   index;
    uint64_t file_size;
    uint64_t position;
    time_t   last_keep_alive;  /* The last time we sent or received data */
    uint32_t line_id;
    uint8_t  file_id[TOX_FILE_ID_LENGTH];
};


struct LastOnline {
    uint64_t last_on;
    struct tm tm;
    char hour_min_str[TIME_STR_SIZE];    /* holds 24-hour time string e.g. "15:43:24" */
};

struct GroupChatInvite {
    char *key;
    uint16_t length;
    uint8_t type;
    bool pending;
};

typedef struct {
    char name[TOXIC_MAX_NAME_LENGTH + 1];
    int namelength;
    char statusmsg[TOX_MAX_STATUS_MESSAGE_LENGTH + 1];
    size_t statusmsg_len;
    char pub_key[TOX_PUBLIC_KEY_SIZE];
    char pubkey_string[(TOX_ADDRESS_SIZE * 2 + 1)];
    char worksubdir[MAX_STR_SIZE];
    uint32_t num;
    bool active;
    TOX_CONNECTION connection_status;
    bool is_typing;
    uint8_t status;
    struct LastOnline last_online;
    int have_resumed_fts; // wait with new FTs until all old FTs have been started (to resume) including avatars!
    struct FileTransfer file_receiver[MAX_FILES];
    struct FileTransfer file_sender[MAX_FILES];
	char last_answer[100];
	int waiting_for_answer; // 0 -> no, 1 -> waiting for answer, 2 -> got answer
	time_t auto_resend_start_time;
	// mz_zip_archive zip_archive;
} ToxicFriend;

typedef struct {
    char name[TOXIC_MAX_NAME_LENGTH + 1];
    int namelength;
    char pub_key[TOX_PUBLIC_KEY_SIZE];
    uint32_t num;
    bool active;
    uint64_t last_on;
} BlockedFriend;

typedef struct {
    int num_selected;
    size_t num_friends;
    size_t num_online;
    size_t max_idx;    /* 1 + the index of the last friend in list */
    uint32_t *index;
    ToxicFriend *list;
} FriendsList;


static struct Avatar {
    char name[TOX_MAX_FILENAME_LENGTH + 1];
    size_t name_len;
    char path[PATH_MAX + 1];
    size_t path_len;
    off_t size;
} Avatar;

typedef struct {
    bool incoming;
    uint32_t state;
	uint32_t audio_bit_rate;
	uint32_t video_bit_rate;
    pthread_mutex_t arb_mutex[1];
} CallControl;


struct buffer {
	void * start;
	size_t length;
};

typedef struct TOXCAM_AV_VIDEO_FRAME {
    uint16_t w, h;
    uint8_t *y, *u, *v;
//    uint8_t bit_depth;
} toxcam_av_video_frame;



void on_avatar_chunk_request(Tox *m, struct FileTransfer *ft, uint64_t position, size_t length);
int avatar_send(Tox *m, uint32_t friendnum);
struct FileTransfer *new_file_transfer(uint32_t friendnum, uint32_t filenum, FILE_TRANSFER_DIRECTION direction, uint8_t type);
void kill_all_file_transfers_friend(Tox *m, uint32_t friendnum);
int has_reached_max_file_transfer_for_friend(uint32_t num);
static int find_friend_in_friendlist(uint32_t friendnum);
int is_friend_online(Tox *tox, uint32_t num);
void av_local_disconnect(ToxAV *av, uint32_t num);
void run_cmd_return_output(const char *command, char* output, int lastline);
void save_resumable_fts(Tox *m, uint32_t friendnum);
void resume_resumable_fts(Tox *m, uint32_t friendnum);
void left_top_bar_into_yuv_frame(int bar_start_x_pix, int bar_start_y_pix, int bar_w_pix, int bar_h_pix, uint8_t r, uint8_t g, uint8_t b);
void print_font_char(int start_x_pix, int start_y_pix, int font_char_num, uint8_t col_value);
void text_on_yuf_frame_xy(int start_x_pix, int start_y_pix, const char* text);
void blinking_dot_on_frame_xy(int start_x_pix, int start_y_pix, int* state);
void black_yuf_frame_xy();
void rbg_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t *y, uint8_t *u, uint8_t *v);
void set_color_in_yuv_frame_xy(uint8_t *yuv_frame, int px_x, int px_y, int frame_w, int frame_h, uint8_t r, uint8_t g, uint8_t b);
void fb_copy_frame_to_fb(void* videoframe);
void fb_fill_black();
void fb_fill_xxx();
void show_video_calling();
void show_text_as_image_stop();
static int get_policy(char p, int *policy);
static void display_thread_sched_attr(char *msg);
static void display_sched_attr(char *msg, int policy, struct sched_param *param);
#ifdef HAVE_ALSA_PLAY
void close_sound_play_device();
void init_sound_play_device(int channels, int sample_rate);
static int sound_play_xrun_recovery(snd_pcm_t *handle, int err, int channels, int sample_rate);
#endif
int64_t friend_number_for_entry(Tox *tox, uint8_t *tox_id_bin);
void bin_to_hex_string(uint8_t *tox_id_bin, size_t tox_id_bin_len, char *toxid_str);
void delete_entry_file(int entry_num);
void call_entry_num(Tox *tox, int entry_num);
void text_on_bgra_frame_xy(int fb_xres, int fb_yres, int fb_line_bytes, uint8_t *fb_buf, int start_x_pix, int start_y_pix, const char* text);
void update_status_line_1_text();
void update_status_line_1_text_arg(int vbr_);


const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";
const char *log_filename = "toxblinkenwall.log";
const char *my_avatar_filename = "avatar.png";
const char *my_toxid_filename_png = "toxid.png";
const char *my_toxid_filename_rgba = "toxid.rgba";
const char *my_toxid_filename_txt = "toxid.txt";
const char *image_createqr_touchfile = "./toxid_ready.txt";

char *v4l2_device; // video device filename
char *framebuffer_device = NULL; // framebuffer device filename

const char *shell_cmd__single_shot = "./scripts/single_shot.sh 2> /dev/null";
const char *shell_cmd__get_cpu_temp = "./scripts/get_cpu_temp.sh 2> /dev/null";
const char *shell_cmd__get_gpu_temp = "./scripts/get_gpu_temp.sh 2> /dev/null";
const char *shell_cmd__get_my_number_of_open_files = "cat /proc/sys/fs/file-nr 2> /dev/null";
const char *shell_cmd__create_qrcode = "./scripts/create_qrcode.sh 2> /dev/null";
const char *shell_cmd__show_qrcode = "./scripts/show_qrcode.sh 2> /dev/null";
const char *shell_cmd__show_clients = "./scripts/show_clients.sh 2> /dev/null";
const char *shell_cmd__start_endless_loading_anim = "./scripts/show_loading_endless_in_bg.sh 2> /dev/null";
const char *shell_cmd__stop_endless_loading_anim = "./scripts/stop_loading_endless.sh 2> /dev/null";
const char *shell_cmd__show_video_calling = "./scripts/show_video_calling.sh 2> /dev/null";
const char *shell_cmd__start_endless_image_anim = "./scripts/show_image_endless_in_bg.sh"; // needs image filename paramter
const char *shell_cmd__stop_endless_image_anim = "./scripts/stop_image_endless.sh 2> /dev/null";
const char *shell_cmd__show_text_as_image = "./scripts/show_text_as_image.sh"; // needs text as parameter. Caution filter out any bad characters!!
const char *shell_cmd__show_text_as_image_stop = "./scripts/show_text_as_image_stop.sh";
const char *cmd__image_filename_full_path = "./tmp/image.dat";
const char *cmd__image_text_full_path = "./tmp/text.dat";
int global_want_restart = 0;
const char *global_timestamp_format = "%H:%M:%S";
const char *global_long_timestamp_format = "%Y-%m-%d %H:%M:%S";
const char *global_overlay_timestamp_format = "%Y-%m-%d %H:%M:%S";
uint64_t global_start_time;
int global_cam_device_fd = 0;
int global_framebuffer_device_fd = 0;
struct fb_var_screeninfo var_framebuffer_info;
struct fb_fix_screeninfo var_framebuffer_fix_info;
size_t framebuffer_screensize = 0;
unsigned char *framebuffer_mappedmem = NULL;
uint32_t n_buffers;
struct buffer *buffers = NULL;
uint16_t video_width = 0;
uint16_t video_height = 0;
struct v4l2_format format;
struct v4l2_format dest_format;
toxcam_av_video_frame av_video_frame;
vpx_image_t input;
int global_video_active = 0;
int global_send_first_frame = 0;
int switch_nodelist_2 = 0;
int video_high = 0;
int switch_tcponly = 0;
int full_width = 640; // gets set later, this is just as last resort
int full_height = 480; // gets set later, this is just as last resort
int vid_width = 192; // ------- blinkenwall resolution -------
int vid_height = 144; // ------- blinkenwall resolution -------
// uint8_t *bf_out_data = NULL; // global buffer, !!please write me better!!

#ifdef HAVE_EXTERNAL_KEYS
int ext_keys_fd;
char *ext_keys_fifo = "ext_keys.fifo";
int do_read_ext_keys = 0;
#define MAX_READ_FIFO_BUF 1000
#endif


#ifdef HAVE_LIBAO
int libao_cancel_pending = 0;
ao_device *_ao_device = NULL;
ao_sample_format _ao_format;
int _ao_default_driver = -1;
sem_t count_audio_play_threads;
int count_audio_play_threads_int;
#define MAX_AO_PLAY_THREADS 3
sem_t audio_play_lock;
#endif

#ifdef HAVE_ALSA_PLAY
snd_pcm_t *audio_play_handle;
const char *audio_play_device = "default";
int have_output_sound_device = 1;
sem_t count_audio_play_threads;
int count_audio_play_threads_int;
#define MAX_ALSA_AUDIO_PLAY_THREADS 1
sem_t audio_play_lock;
#define ALSA_AUDIO_PLAY_START_THRESHOLD (10)
#define ALSA_AUDIO_PLAY_SILENCE_THRESHOLD (2)
#endif


sem_t count_video_play_threads;
int count_video_play_threads_int;
#define MAX_VIDEO_PLAY_THREADS 2
sem_t video_play_lock;

uint16_t video__width;
uint16_t video__height;
uint8_t const *video__y;
uint8_t const *video__u;
uint8_t const *video__v;
int32_t video__ystride;
int32_t video__ustride;
int32_t video__vstride;


#define MAX_VIDEO_RECORD_THREADS 1
sem_t count_video_record_threads;
int count_video_record_threads_int;



#ifdef HAVE_PORTAUDIO
PaStream *portaudio_stream = NULL;
ringbuf_t portaudio_out_rb;
#endif


#define DEFAULT_AUDIO_CAPTURE_SAMPLERATE (48000)
// Valid sampling rates are: 8000, 12000, 16000, 24000, or 48000
#define DEFAULT_AUDIO_CAPTURE_CHANNELS (1)

#ifdef HAVE_ALSA_REC
snd_pcm_t *audio_capture_handle;
// const char *audio_device = "plughw:0,0";
// const char *audio_device = "hw:CARD=U0x46d0x933,DEV=0";
const char *audio_device = "default";
// sysdefault:CARD
int do_audio_recording = 1;
int have_input_sound_device = 1;
#define MAX_ALSA_RECORD_THREADS 4
#define AUDIO_RECORD_AUDIO_LENGTH (120)
// frames = ((sample rate) * (audio length) / 1000)  -> audio length: [ 2.5, 5, 10, 20, 40 or 60 ] (120 seems to work also, 240 does NOT)
// frame is also = ((AUDIO_RECORD_BUFFER_BYTES / DEFAULT_AUDIO_CAPTURE_CHANNELS) / 2)
#define AUDIO_RECORD_BUFFER_FRAMES (((DEFAULT_AUDIO_CAPTURE_SAMPLERATE) * (AUDIO_RECORD_AUDIO_LENGTH)) / 1000)
#define AUDIO_RECORD_BUFFER_BYTES (AUDIO_RECORD_BUFFER_FRAMES * 2)
sem_t count_audio_record_threads;
int count_audio_record_threads_int;
#endif

uint32_t global_audio_bit_rate;
uint32_t global_video_bit_rate;
ToxAV *mytox_av = NULL;
int tox_loop_running = 1;
int global_blink_state = 0;

uint8_t libao_channels = 2;
uint32_t libao_sampling_rate = 1;
char *ao_play_pcm;
size_t ao_play_bytes;

int toxav_video_thread_stop = 0;
int toxav_iterate_thread_stop = 0;

int incoming_filetransfers = 0;
uint32_t incoming_filetransfers_friendnumber = -1;
uint32_t incoming_filetransfers_filenumber = -1;
int global_is_qrcode_showing_on_screen = 0;
int global_qrcode_was_updated = 0;
int toggle_display_frame = 0;
int SHOW_EVERY_X_TH_VIDEO_FRAME = 1;

// -- hardcoded --
// -- hardcoded --
// -- hardcoded --
uint32_t friend_to_send_video_to = -1;
// -- hardcoded --
// -- hardcoded --
// -- hardcoded --

int video_call_enabled = 1;
int accepting_calls = 0;
int global_show_fps_on_video = 0;
char status_line_1_str[200];
char status_line_2_str[200];
uint32_t global_video_in_fps;
uint32_t global_video_out_fps;
int update_fps_every = 20;
int update_fps_counter = 0;
const char *speaker_out_name_0 = "TV ";
const char *speaker_out_name_1 = "SPK";
int speaker_out_num = 0;


TOX_CONNECTION my_connection_status = TOX_CONNECTION_NONE;
FILE *logfile = NULL;
FriendsList Friends;



void dbg__X(int level, const char *fmt, ...)
{
	// DUMMY
}

void dbg(int level, const char *fmt, ...)
{
    char *level_and_format = NULL;
    char *fmt_copy = NULL;

    if (fmt == NULL)
    {
        return;
    }

    if (strlen(fmt) < 1)
    {
        return;
    }

    if (!logfile)
    {
        return;
    }

    if ((level < 0) || (level > 9))
    {
        level = 0;
    }

    level_and_format = malloc(strlen(fmt) + 3 + 1);

    if (!level_and_format)
    {
        // dbg(9, stderr, "free:000a\n");
        return;
    }

    fmt_copy = level_and_format + 2;
    strcpy(fmt_copy, fmt);
    level_and_format[1] = ':';
    if (level == 0)
    {
        level_and_format[0] = 'E';
    }
    else if (level == 1)
    {
        level_and_format[0] = 'W';
    }
    else if (level == 2)
    {
        level_and_format[0] = 'I';
    }
    else
    {
        level_and_format[0] = 'D';
    }

    level_and_format[(strlen(fmt) + 2)] = '\0'; // '\0' or '\n'
    level_and_format[(strlen(fmt) + 3)] = '\0';

    time_t t3 = time(NULL);
    struct tm tm3 = *localtime(&t3);

    char *level_and_format_2 = malloc(strlen(level_and_format) + 5 + 3 + 3 + 1 + 3 + 3 + 3 + 1);
    level_and_format_2[0] = '\0';
    snprintf(level_and_format_2, (strlen(level_and_format) + 5 + 3 + 3 + 1 + 3 + 3 + 3 + 1),
             "%04d-%02d-%02d %02d:%02d:%02d:%s",
             tm3.tm_year + 1900, tm3.tm_mon + 1, tm3.tm_mday,
             tm3.tm_hour, tm3.tm_min, tm3.tm_sec, level_and_format);

    if (level <= CURRENT_LOG_LEVEL)
    {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(logfile, level_and_format_2, ap);
        va_end(ap);
    }

    // dbg(9, "free:001\n");
    if (level_and_format)
    {
        // dbg(9, "free:001.a\n");
        free(level_and_format);
    }

    if (level_and_format_2)
    {
        free(level_and_format_2);
    }
    // dbg(9, "free:002\n");
}

static inline void __utimer_start(struct timeval* tm1)
{
    gettimeofday(tm1, NULL);
}

static inline unsigned long long __utimer_stop(struct timeval* tm1, const char* log_msg, int no_log)
{
    struct timeval tm2;
    gettimeofday(&tm2, NULL);

    unsigned long long t = 1000 * (tm2.tv_sec - tm1->tv_sec) + (tm2.tv_usec - tm1->tv_usec) / 1000;
	if (no_log == 0)
	{
		dbg(9, "%s %llu ms\n", log_msg, t);
	}
	return t;
}

uint32_t generate_random_uint32()
{
	uint32_t r;
	struct timeval time;
	gettimeofday(&time,NULL);
	srand((time.tv_sec * 1000) + (time.tv_usec / 1000));

	rand();
	rand();
	r = rand();
	return r;
}

unsigned int char_to_int(char c)
{
    if (c >= '0' && c <= '9')
    { return c - '0'; }
    if (c >= 'A' && c <= 'F')
    { return 10 + c - 'A'; }
    if (c >= 'a' && c <= 'f')
    { return 10 + c - 'a'; }
    return -1;
}

void bin_to_hex_string(uint8_t *tox_id_bin, size_t tox_id_bin_len, char *toxid_str)
{
    char tox_id_hex_local[TOX_ADDRESS_SIZE * 2 + 1];
    CLEAR(tox_id_hex_local);

    // dbg(9, "bin_to_hex_string:sizeof(tox_id_hex_local)=%d\n", (int)sizeof(tox_id_hex_local));
    // dbg(9, "bin_to_hex_string:strlen(tox_id_bin)=%d\n", (int)tox_id_bin_len);

    sodium_bin2hex(tox_id_hex_local, sizeof(tox_id_hex_local), tox_id_bin, tox_id_bin_len);

    for (size_t i = 0; i < sizeof(tox_id_hex_local) - 1; i++)
    {
        // dbg(9, "i=%d\n", i);
        tox_id_hex_local[i] = toupper(tox_id_hex_local[i]);
    }

    snprintf(toxid_str, (size_t) (TOX_ADDRESS_SIZE * 2 + 1), "%s", (const char *) tox_id_hex_local);
}

/*
 * Converts a hexidecimal string of length hex_len to binary format and puts the result in output.
 * output_size must be exactly half of hex_len.
 *
 * Returns 0 on success.
 * Returns -1 on failure.
 */
uint8_t *hex_string_to_bin(const char *hex_string)
{
    size_t len = TOX_ADDRESS_SIZE;
    uint8_t *val = malloc(len);
    memset(val, 0, (size_t) len);

    // dbg(9, "hex_string_to_bin:len=%d\n", (int)len);

    for (size_t i = 0; i != len; ++i)
    {
        // dbg(9, "hex_string_to_bin:%d %d\n", hex_string[2*i], hex_string[2*i+1]);
        val[i] = (16 * char_to_int(hex_string[2 * i])) + (char_to_int(hex_string[2 * i + 1]));
        // dbg(9, "hex_string_to_bin:i=%d val[i]=%d\n", i, (int)val[i]);
    }

    return val;
}


/* Converts a binary representation of a Tox ID into a string.
 *
 * Returns 0 on success.
 * Returns -1 on failure.
 */
int bin_id_to_string(const char *bin_id, size_t bin_id_size, char *output, size_t output_size)
{
    if (bin_id_size != TOX_ADDRESS_SIZE || output_size < (TOX_ADDRESS_SIZE * 2 + 1))
    {
        return -1;
    }

    size_t i;
    for (i = 0; i < TOX_ADDRESS_SIZE; ++i)
    {
        snprintf(&output[i * 2], output_size - (i * 2), "%02X", bin_id[i] & 0xff);
    }

	return 0;
}






// linked list ----------
typedef struct ll_node {
    void* val;
    struct ll_node* next;
} ll_node_t;


void ll_fill_val(void **val, size_t data_size, void* data)
{
	if (*val != NULL)
	{
		free(*val);
		*val = NULL;
	}

	*val = malloc(data_size);
	memcpy(*val, data, data_size);
}


// add to the beginning of the list
void ll_push(ll_node_t** head, size_t data_size, void* data)
{
    ll_node_t* new_node;
    new_node = calloc(1, sizeof(ll_node_t));
	ll_fill_val(&(new_node->val), data_size, data);
    new_node->next = *head;
    *head = new_node;
}

void* ll_pop(ll_node_t** head)
{
    void* retval = NULL;
    ll_node_t* next_node = NULL;

    if (*head == NULL)
	{
        return NULL;
	}

    next_node = (*head)->next;
    retval = (*head)->val;
    free(*head);
    *head = next_node;

    return retval;
}

void ll_free_val(void* val)
{
	if (val != NULL)
	{
		free(val);
		val = NULL;
	}
}

void* ll_remove_by_index(ll_node_t** head, int n)
{
    int i = 0;
    void* retval = NULL;
    ll_node_t* current = *head;
    ll_node_t* temp_node = NULL;

    if (n == 0)
	{
        return ll_pop(head);
    }

    for (i = 0; i < n-1; i++)
        {
        if (current->next == NULL)
                {
            return NULL;
        }
        current = current->next;
    }

    temp_node = current->next;
	if (temp_node != NULL)
	{
			retval = temp_node->val;
			current->next = temp_node->next;
			free(temp_node);
	}

    return retval;
}

#if 0
void ll_print_list(ll_node_t* head)
{
    ll_node_t* current = head;
    int i = 0;

    while (current != NULL)
	{
		dbg(9, "element #%d=%p\n", i, current->val);
		i++;
		current = current->next;
	}
}
#endif

ll_node_t* resumable_filetransfers = NULL;

// linked list ----------









time_t get_unix_time(void)
{
    return time(NULL);
}

void yieldcpu(uint32_t ms)
{
    usleep(1000 * ms);
}

int get_number_in_string(const char *str, int default_value)
{
    int number;

    while (!(*str >= '0' && *str <= '9') && (*str != '-') && (*str != '+')) str++;

    if (sscanf(str, "%d", &number) == 1)
    {
        return number;
    }

    // no int found, return default value
    return default_value; 
}


void tox_log_cb__custom(Tox *tox, TOX_LOG_LEVEL level, const char *file, uint32_t line, const char *func, const char *message, void *user_data)
{
	dbg(9, "%d:%s:%d:%s:%s\n", (int)level, file, (int)line, func, message);
}


#ifdef HAVE_PORTAUDIO

/* This routine will be called by the PortAudio engine when audio is needed */
static int portaudio_data_callback( const void *inputBuffer,
                            void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData )
{

	#define SAMPLE_SILENCE (0)
	int16_t *out = (int16_t*)outputBuffer;

	size_t have_bytes_in_buffer = ringbuf_bytes_used(portaudio_out_rb);
	size_t have_frames = (size_t)((have_bytes_in_buffer / libao_channels) / 2);
	dbg(9, "have_bytes_in_buffer=%d framesPerBuffer=%d\n", (int)have_bytes_in_buffer, (int)framesPerBuffer);
	dbg(9, "have_frames=%d\n", (int)have_frames);

	if (have_frames <= framesPerBuffer)
	{
		ringbuf_memcpy_from(out, portaudio_out_rb, (size_t)(framesPerBuffer * libao_channels * 2));
#if 1
		unsigned long i;
		for( i=0; i < framesPerBuffer; i++ )
		{
			// we don't have enough data, fill up with silence ..
			if (i > have_frames)
			{
				*out++ = SAMPLE_SILENCE;
				if (libao_channels == 2)
				{
					*out++ = SAMPLE_SILENCE;
				}
			}
		}
#endif
	}
	else
	{
		ringbuf_memcpy_from(out, portaudio_out_rb, (size_t)(framesPerBuffer * channels * 2));
	}

	// unsigned long i;
	// for( i=0; i < framesPerBuffer; i++ )
	// {
	//	// *out++ = i % 16383;
	// }

	return paContinue;
}

/*
 * This routine is called by portaudio when playback is done.
 */
static void StreamFinished( void* userData )
{
	dbg(2, "Audio Stream Completed\n");
}
#endif



Tox *create_tox()
{
	Tox *tox;
	struct Tox_Options options;

/*
	TOX_ERR_OPTIONS_NEW err_options;
    struct Tox_Options options = tox_options_new(&err_options);
	if (err_options != TOX_ERR_OPTIONS_NEW_OK)
	{
		dbg(0, "tox_options_new error\n");
	}
*/

	tox_options_default(&options);

	// ----------------------------------------------
	// uint16_t tcp_port = 33445; // act as TCP relay
	uint16_t tcp_port = 0; // DON'T act as TCP relay
	// ----------------------------------------------

	// ----------------------------------------------
	if (switch_tcponly == 0)
	{
		options.udp_enabled = true; // UDP mode
		dbg(0, "setting UDP mode\n");
	}
	else
	{
		options.udp_enabled = false; // TCP mode
		dbg(0, "setting TCP mode\n");
	}
	// ----------------------------------------------

	options.ipv6_enabled = false;
	options.local_discovery_enabled = true;
	options.hole_punching_enabled = true;
	options.tcp_port = tcp_port;

	// ------------------------------------------------------------
	// set our own handler for c-toxcore logging messages!!
	options.log_callback = tox_log_cb__custom;
	// ------------------------------------------------------------


    FILE *f = fopen(savedata_filename, "rb");
    if (f)
	{
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        uint8_t *savedata = malloc(fsize);

        size_t dummy = fread(savedata, fsize, 1, f);
		if (dummy < 1)
		{
			dbg(0, "reading savedata failed\n");
		}
        fclose(f);

        options.savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
        options.savedata_data = savedata;
        options.savedata_length = fsize;

        tox = tox_new(&options, NULL);

        free((void *)savedata);
    }
	else
	{
        tox = tox_new(&options, NULL);
    }

	bool local_discovery_enabled = tox_options_get_local_discovery_enabled(&options);
	dbg(9, "local discovery enabled = %d\n", (int)local_discovery_enabled);

    return tox;
}

void replace_bad_char_from_string(char *str, const char replace_with)
{
	// win32: '\ / : * ? " < > |'
	char bad_chars[] = "/:*?<>|\"";
	int i;
	int j;

	if ((str) && (strlen(str) > 0))
	{
		for(i = 0; (int)i < (int)strlen(str) ;i++)
		{
			for(j = 0; (int)j < (int)strlen(bad_chars); j++)
			if (str[i] == bad_chars[j])
			{
				str[i] = replace_with;
			}
		}
	}
}


void update_savedata_file(const Tox *tox)
{
    size_t size = tox_get_savedata_size(tox);
    char *savedata = malloc(size);
    tox_get_savedata(tox, (uint8_t *)savedata);

    FILE *f = fopen(savedata_tmp_filename, "wb");
    fwrite(savedata, size, 1, f);
    fclose(f);

    rename(savedata_tmp_filename, savedata_filename);

    free(savedata);
}

off_t file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
    {
        return 0;
    }

    return st.st_size;
}


void random_char(char *output, int len)
{
	int i;
	srandom(time(NULL));

	for (i = 0; i < len - 1; i++)
	{
		output[i] = (unsigned char) (rand() % 255 + 1);
	}
	output[len - 1] = '\0';
}

void bin_id_to_string_all(const char *bin_id, size_t bin_id_size, char *output, size_t output_size)
{
	if (bin_id)
	{
		size_t i;
		for (i = 0; i < bin_id_size; ++i)
		{
			snprintf(&output[i * 2], output_size - (i * 2), "%02X", bin_id[i] & 0xff);
		}
	}
}


size_t get_file_name(char *namebuf, size_t bufsize, const char *pathname)
{
    int len = strlen(pathname) - 1;
    char *path = strdup(pathname);

    if (path == NULL)
    {
        // TODO
    }

    while (len >= 0 && pathname[len] == '/')
    {
        path[len--] = '\0';
    }

    char *finalname = strdup(path);

    if (finalname == NULL)
    {
        // TODO
    }

    const char *basenm = strrchr(path, '/');
    if (basenm != NULL)
    {
        if (basenm[1])
        {
            strcpy(finalname, &basenm[1]);
        }
    }

    snprintf(namebuf, bufsize, "%s", finalname);
    free(finalname);
    free(path);

    return strlen(namebuf);
}

void bootstap_nodes(Tox *tox, DHT_node nodes[], int number_of_nodes, int add_as_tcp_relay)
{

	bool res = 0;
    for (size_t i = 0; (int)i < (int)number_of_nodes; i ++)
	{
        res = sodium_hex2bin(nodes[i].key_bin, sizeof(nodes[i].key_bin),
                       nodes[i].key_hex, sizeof(nodes[i].key_hex)-1, NULL, NULL, NULL);
		// dbg(9, "sodium_hex2bin:res=%d\n", res);

		TOX_ERR_BOOTSTRAP error;
        res = tox_bootstrap(tox, nodes[i].ip, nodes[i].port, nodes[i].key_bin, &error);
		if (res != true)
		{
			if (error == TOX_ERR_BOOTSTRAP_OK)
			{
				// dbg(9, "bootstrap:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_OK\n", nodes[i].ip, nodes[i].port);
			}
			else if (error == TOX_ERR_BOOTSTRAP_NULL)
			{
				// dbg(9, "bootstrap:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_NULL\n", nodes[i].ip, nodes[i].port);
			}
			else if (error == TOX_ERR_BOOTSTRAP_BAD_HOST)
			{
				// dbg(9, "bootstrap:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_BAD_HOST\n", nodes[i].ip, nodes[i].port);
			}
			else if (error == TOX_ERR_BOOTSTRAP_BAD_PORT)
			{
				// dbg(9, "bootstrap:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_BAD_PORT\n", nodes[i].ip, nodes[i].port);
			}
		}
		else
		{
			// dbg(9, "bootstrap:%s %d [TRUE]res=%d\n", nodes[i].ip, nodes[i].port, res);
		}

		if (add_as_tcp_relay == 1)
		{
			res = tox_add_tcp_relay(tox, nodes[i].ip, nodes[i].port, nodes[i].key_bin, &error); // use also as TCP relay
			if (res != true)
			{
				if (error == TOX_ERR_BOOTSTRAP_OK)
				{
					// dbg(9, "add_tcp_relay:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_OK\n", nodes[i].ip, nodes[i].port);
				}
				else if (error == TOX_ERR_BOOTSTRAP_NULL)
				{
					// dbg(9, "add_tcp_relay:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_NULL\n", nodes[i].ip, nodes[i].port);
				}
				else if (error == TOX_ERR_BOOTSTRAP_BAD_HOST)
				{
					// dbg(9, "add_tcp_relay:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_BAD_HOST\n", nodes[i].ip, nodes[i].port);
				}
				else if (error == TOX_ERR_BOOTSTRAP_BAD_PORT)
				{
					// dbg(9, "add_tcp_relay:%s %d [FALSE]res=TOX_ERR_BOOTSTRAP_BAD_PORT\n", nodes[i].ip, nodes[i].port);
				}
			}
			else
			{
				// dbg(9, "add_tcp_relay:%s %d [TRUE]res=%d\n", nodes[i].ip, nodes[i].port, res);
			}
		}
		else
		{
			dbg(2, "Not adding any TCP relays\n");
		}
    }
}


void bootstrap(Tox *tox)
{

	// these nodes seem to be faster!!
    DHT_node nodes1[] =
    {
        {"178.62.250.138",             33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", {0}},
        {"51.15.37.145",             33445, "6FC41E2BD381D37E9748FC0E0328CE086AF9598BECC8FEB7DDF2E440475F300E", {0}},
        {"130.133.110.14",             33445, "461FA3776EF0FA655F1A05477DF1B3B614F7D6B124F7DB1DD4FE3C08B03B640F", {0}},
        {"23.226.230.47",         33445, "A09162D68618E742FFBCA1C2C70385E6679604B2D80EA6E84AD0996A1AC8A074", {0}},
        {"163.172.136.118",            33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", {0}},
        {"217.182.143.254",             443, "7AED21F94D82B05774F697B209628CD5A9AD17E0C073D9329076A4C28ED28147", {0}},
        {"185.14.30.213",               443,  "2555763C8C460495B14157D234DD56B86300A2395554BCAE4621AC345B8C1B1B", {0}},
        {"136.243.141.187",             443,  "6EE1FADE9F55CC7938234CC07C864081FC606D8FE7B751EDA217F268F1078A39", {0}},
        {"128.199.199.197",            33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", {0}},
        {"198.46.138.44",               33445, "F404ABAA1C99A9D37D61AB54898F56793E1DEF8BD46B1038B9D822E8460FAB67", {0}}
    };


	// more nodes here, but maybe some issues
    DHT_node nodes2[] =
    {
        {"178.62.250.138",             33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", {0}},
        {"136.243.141.187",             443,  "6EE1FADE9F55CC7938234CC07C864081FC606D8FE7B751EDA217F268F1078A39", {0}},
        {"185.14.30.213",               443,  "2555763C8C460495B14157D234DD56B86300A2395554BCAE4621AC345B8C1B1B", {0}},
		{"198.46.138.44",33445,"F404ABAA1C99A9D37D61AB54898F56793E1DEF8BD46B1038B9D822E8460FAB67", {0}},
		{"51.15.37.145",33445,"6FC41E2BD381D37E9748FC0E0328CE086AF9598BECC8FEB7DDF2E440475F300E", {0}},
		{"130.133.110.14",33445,"461FA3776EF0FA655F1A05477DF1B3B614F7D6B124F7DB1DD4FE3C08B03B640F", {0}},
		{"205.185.116.116",33445,"A179B09749AC826FF01F37A9613F6B57118AE014D4196A0E1105A98F93A54702", {0}},
		{"198.98.51.198",33445,"1D5A5F2F5D6233058BF0259B09622FB40B482E4FA0931EB8FD3AB8E7BF7DAF6F", {0}},
		{"108.61.165.198",33445,"8E7D0B859922EF569298B4D261A8CCB5FEA14FB91ED412A7603A585A25698832", {0}},
		{"194.249.212.109",33445,"3CEE1F054081E7A011234883BC4FC39F661A55B73637A5AC293DDF1251D9432B", {0}},
		{"185.25.116.107",33445,"DA4E4ED4B697F2E9B000EEFE3A34B554ACD3F45F5C96EAEA2516DD7FF9AF7B43", {0}},
		{"5.189.176.217",5190,"2B2137E094F743AC8BD44652C55F41DFACC502F125E99E4FE24D40537489E32F", {0}},
		{"217.182.143.254",2306,"7AED21F94D82B05774F697B209628CD5A9AD17E0C073D9329076A4C28ED28147", {0}},
		{"104.223.122.15",33445,"0FB96EEBFB1650DDB52E70CF773DDFCABE25A95CC3BB50FC251082E4B63EF82A", {0}},
		{"tox.verdict.gg",33445,"1C5293AEF2114717547B39DA8EA6F1E331E5E358B35F9B6B5F19317911C5F976", {0}},
		{"d4rk4.ru",1813,"53737F6D47FA6BD2808F378E339AF45BF86F39B64E79D6D491C53A1D522E7039", {0}},
		{"104.233.104.126",33445,"EDEE8F2E839A57820DE3DA4156D88350E53D4161447068A3457EE8F59F362414", {0}},
		{"51.254.84.212",33445,"AEC204B9A4501412D5F0BB67D9C81B5DB3EE6ADA64122D32A3E9B093D544327D", {0}},
		{"88.99.133.52",33445,"2D320F971EF2CA18004416C2AAE7BA52BF7949DB34EA8E2E21AF67BD367BE211", {0}},
		{"185.58.206.164",33445,"24156472041E5F220D1FA11D9DF32F7AD697D59845701CDD7BE7D1785EB9DB39", {0}},
		{"92.54.84.70",33445,"5625A62618CB4FCA70E147A71B29695F38CC65FF0CBD68AD46254585BE564802", {0}},
		{"195.93.190.6",33445,"FB4CE0DDEFEED45F26917053E5D24BDDA0FA0A3D83A672A9DA2375928B37023D", {0}},
		{"tox.uplinklabs.net",33445,"1A56EA3EDF5DF4C0AEABBF3C2E4E603890F87E983CAC8A0D532A335F2C6E3E1F", {0}},
		{"toxnode.nek0.net",33445,"20965721D32CE50C3E837DD75B33908B33037E6225110BFF209277AEAF3F9639", {0}},
		{"95.215.44.78",33445,"672DBE27B4ADB9D5FB105A6BB648B2F8FDB89B3323486A7A21968316E012023C", {0}},
		{"163.172.136.118",33445,"2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", {0}},
		{"sorunome.de",33445,"02807CF4F8BB8FB390CC3794BDF1E8449E9A8392C5D3F2200019DA9F1E812E46", {0}},
		{"37.97.185.116",33445,"E59A0E71ADA20D35BD1B0957059D7EF7E7792B3D680AE25C6F4DBBA09114D165", {0}},
		{"193.124.186.205",5228,"9906D65F2A4751068A59D30505C5FC8AE1A95E0843AE9372EAFA3BAB6AC16C2C", {0}},
		{"80.87.193.193",33445,"B38255EE4B054924F6D79A5E6E5889EC94B6ADF6FE9906F97A3D01E3D083223A", {0}},
		{"initramfs.io",33445,"3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25", {0}},
		{"hibiki.eve.moe",33445,"D3EB45181B343C2C222A5BCF72B760638E15ED87904625AAD351C594EEFAE03E", {0}},
		{"tox.deadteam.org",33445,"C7D284129E83877D63591F14B3F658D77FF9BA9BA7293AEB2BDFBFE1A803AF47", {0}},
		{"46.229.52.198",33445,"813C8F4187833EF0655B10F7752141A352248462A567529A38B6BBF73E979307", {0}},
		{"node.tox.ngc.network",33445,"A856243058D1DE633379508ADCAFCF944E40E1672FF402750EF712E30C42012A", {0}},
		{"144.217.86.39",33445,"7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C", {0}},
		{"185.14.30.213",443,"2555763C8C460495B14157D234DD56B86300A2395554BCAE4621AC345B8C1B1B", {0}},
		{"77.37.160.178",33440,"CE678DEAFA29182EFD1B0C5B9BC6999E5A20B50A1A6EC18B91C8EBB591712416", {0}},
		{"85.21.144.224",33445,"8F738BBC8FA9394670BCAB146C67A507B9907C8E564E28C2B59BEBB2FF68711B", {0}},
		{"tox.natalenko.name",33445,"1CB6EBFD9D85448FA70D3CAE1220B76BF6FCE911B46ACDCF88054C190589650B", {0}},
		{"37.187.122.30",33445,"BEB71F97ED9C99C04B8489BB75579EB4DC6AB6F441B603D63533122F1858B51D", {0}},
		{"completelyunoriginal.moe",33445,"FBC7DED0B0B662D81094D91CC312D6CDF12A7B16C7FFB93817143116B510C13E", {0}},
		{"136.243.141.187",443,"6EE1FADE9F55CC7938234CC07C864081FC606D8FE7B751EDA217F268F1078A39", {0}},
		{"tox.abilinski.com",33445,"0E9D7FEE2AA4B42A4C18FE81C038E32FFD8D907AAA7896F05AA76C8D31A20065", {0}},
		{"95.215.46.114",33445,"5823FB947FF24CF83DDFAC3F3BAA18F96EA2018B16CC08429CB97FA502F40C23", {0}},
		{"51.15.54.207",33445,"1E64DBA45EC810C0BF3A96327DC8A9D441AB262C14E57FCE11ECBCE355305239", {0}}
    };

	// only nodes.tox.chat
    DHT_node nodes3[] =
    {
        {"51.15.37.145",             33445, "6FC41E2BD381D37E9748FC0E0328CE086AF9598BECC8FEB7DDF2E440475F300E", {0}}
    };


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
	if (switch_nodelist_2 == 0)
	{
		dbg(9, "nodeslist:1\n");
		bootstap_nodes(tox, nodes1, (int)(sizeof(nodes1)/sizeof(DHT_node)), 1);
	}
	else if (switch_nodelist_2 == 2)
	{
		dbg(9, "nodeslist:3\n");
		bootstap_nodes(tox, nodes3, (int)(sizeof(nodes3)/sizeof(DHT_node)), 0);
	}
	else // (switch_nodelist_2 == 1)
	{
		dbg(9, "nodeslist:2\n");
		bootstap_nodes(tox, nodes2, (int)(sizeof(nodes2)/sizeof(DHT_node)), 1);
	}
#pragma GCC diagnostic pop

}


// fill string with toxid in upper case hex.
// size of toxid_str needs to be: [TOX_ADDRESS_SIZE*2 + 1] !!
void get_my_toxid(Tox *tox, char *toxid_str)
{
    uint8_t tox_id_bin[TOX_ADDRESS_SIZE];
    tox_self_get_address(tox, tox_id_bin);

	char tox_id_hex_local[TOX_ADDRESS_SIZE*2 + 1];
    sodium_bin2hex(tox_id_hex_local, sizeof(tox_id_hex_local), tox_id_bin, sizeof(tox_id_bin));

    for (size_t i = 0; i < sizeof(tox_id_hex_local)-1; i ++)
	{
        tox_id_hex_local[i] = toupper(tox_id_hex_local[i]);
    }

	snprintf(toxid_str, (size_t)(TOX_ADDRESS_SIZE*2 + 1), "%s", (const char*)tox_id_hex_local);
}

void print_tox_id(Tox *tox)
{
    char tox_id_hex[TOX_ADDRESS_SIZE*2 + 1];
	get_my_toxid(tox, tox_id_hex);

    if (logfile)
    {
		dbg(2, "--MyToxID--:%s\n", tox_id_hex);
        int fd = fileno(logfile);
        fsync(fd);
    }

	// write ToxID to toxid text file -----------
	FILE *fp = fopen(my_toxid_filename_txt, "wb");
	if (fp)
	{
		fprintf(fp, "%s", tox_id_hex);
		fclose(fp);
	}
	// write ToxID to toxid text file -----------
}

void show_video_calling()
{
	char cmd_str[1000];
    CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__show_video_calling);
	if (system(cmd_str));

	yieldcpu(600);
}

void show_text_as_image(const char *display_text)
{
	char cmd_str[1000];
	CLEAR(cmd_str);

	int MAX_TEXT_ON_IMAGE_LEN = 300;

	char display_text2[MAX_TEXT_ON_IMAGE_LEN + 10];
	CLEAR(display_text2);

	const char safe_char = ' ';
	const char* s = display_text;
	s=s + 6; // remove leading ".text " from input string

	int i=0;
	while (*s)
	{
		if (i > MAX_TEXT_ON_IMAGE_LEN)
		{
			break;
		}

		if (*s == '&')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == '"')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == '\\')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == '\'')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == '%')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == '|')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == ';')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else if (*s == '_')
		{
			display_text2[i]= safe_char;
			i++;
		}
		else
		{
			display_text2[i]= *s;
			i++;
		}
		s++;
	}

	// display_text2[i] = '\0';

	dbg(0, "in=%s out=%s\n", display_text, display_text2);
	if ((display_text) && (display_text2))
	{
		dbg(0, "in=%d out=%d\n", (int)strlen(display_text), (int)strlen(display_text2));
	}

	FILE *fp = fopen(cmd__image_text_full_path, "ab");
	if (fp != NULL)
	{
		fputs(display_text2, fp);
		fclose(fp);

		// snprintf(cmd_str, sizeof(cmd_str), "%s '%s'", shell_cmd__show_text_as_image, display_text2);
		snprintf(cmd_str, sizeof(cmd_str), "%s ''", shell_cmd__show_text_as_image);
		if (system(cmd_str));

		// unlink(cmd__image_text_full_path);
	}
}

void show_text_as_image_stop()
{
	char cmd_str[1000];
	CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__show_text_as_image_stop);
	if (system(cmd_str));
}

void show_endless_image()
{
	global_is_qrcode_showing_on_screen = 0;

	show_text_as_image_stop();
	
	char cmd_str[1000];
	CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s \"%s\"", shell_cmd__start_endless_image_anim, cmd__image_filename_full_path);
	if (system(cmd_str));
}

void stop_endless_image()
{
	char cmd_str[1000];
    CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__stop_endless_image_anim);
	if (system(cmd_str));
}


void show_endless_loading()
{
	global_is_qrcode_showing_on_screen = 0;

	show_text_as_image_stop();

	char cmd_str[1000];
    CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__start_endless_loading_anim);
	if (system(cmd_str));
}

void stop_endless_loading()
{
	char cmd_str[1000];
    CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__stop_endless_loading_anim);
	if (system(cmd_str));
}

void create_tox_id_qrcode(Tox *tox)
{
    char tox_id_hex[TOX_ADDRESS_SIZE*2 + 1];
	get_my_toxid(tox, tox_id_hex);

	dbg(2, "ToxID:%s\n", tox_id_hex);

	char cmd_str[1000];
    CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s \"tox:%s\"", shell_cmd__create_qrcode, tox_id_hex);
	if (system(cmd_str));

	global_qrcode_was_updated = 1;
}

void delete_qrcode_generate_touchfile()
{
	unlink(image_createqr_touchfile);
}

int is_qrcode_generated()
{
	int is_ready = 0;

	FILE *file = NULL;
	file = fopen(image_createqr_touchfile, "r");
	if (file)
	{
		fclose(file);
		is_ready = 1;
	}
	else
	{
	}

	return is_ready;
}

void show_tox_id_qrcode()
{
	show_text_as_image_stop();

	char cmd_str[1000];
	CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__show_qrcode);
	if (system(cmd_str));

	dbg(2, "show_tox_id_qrcode()\n");

	global_is_qrcode_showing_on_screen = 1;
}

void show_tox_client_application_download_links()
{
	show_text_as_image_stop();

	char cmd_str[1000];
    CLEAR(cmd_str);
	snprintf(cmd_str, sizeof(cmd_str), "%s", shell_cmd__show_clients);
	if (system(cmd_str));
}


int is_friend_online(Tox *tox, uint32_t num)
{
	int j = find_friend_in_friendlist(num);
	switch (Friends.list[j].connection_status)
	{
		case TOX_CONNECTION_NONE:
			return 0;
			break;
		case TOX_CONNECTION_TCP:
			return 1;
			break;
		case TOX_CONNECTION_UDP:
			return 1;
			break;
		default:
			return 0;
			break;
	}
}

static int find_friend_in_friendlist(uint32_t friendnum)
{
	int i;

	for (i = 0; i <= Friends.max_idx; ++i)
	{
        if (Friends.list[i].num == friendnum)
		{
			return i;
		}
	}

	return -1;
}

static void update_friend_last_online(uint32_t num, time_t timestamp)
{
	int friendlistnum = find_friend_in_friendlist(num);

    Friends.list[friendlistnum].last_online.last_on = timestamp;
    Friends.list[friendlistnum].last_online.tm = *localtime((const time_t *)&timestamp);

    /* if the format changes make sure TIME_STR_SIZE is the correct size !! */
    strftime(Friends.list[friendlistnum].last_online.hour_min_str, TIME_STR_SIZE, global_timestamp_format, &Friends.list[friendlistnum].last_online.tm);
}

void send_file_to_friend_real(Tox *m, uint32_t num, const char* filename, int resume, uint8_t* fileid_resume)
{
    // ------- hack to send file --------
    // ------- hack to send file --------
    const char *errmsg = NULL;
    char path[MAX_STR_SIZE];

    snprintf(path, sizeof(path), "%s", filename);
    dbg(2, "send_file_to_friend_real:path=%s\n", path);

    FILE *file_to_send = fopen(path, "r");

    if (file_to_send == NULL)
    {
		dbg(0, "send_file_to_friend_real:error opening file\n");
		return;
    }

    off_t filesize = file_size(path);

    if (filesize == 0)
    {
		dbg(0, "send_file_to_friend_real:filesize 0\n");
		fclose(file_to_send);
		return;
    }

    char file_name[TOX_MAX_FILENAME_LENGTH];
    size_t namelen = get_file_name(file_name, sizeof(file_name), path);

    TOX_ERR_FILE_SEND err;

	char *o = calloc(1, (size_t)TOX_FILE_ID_LENGTH);
	uint32_t filenum = -1;
	if (resume == 0)
	{
		dbg(9, "resume == 0\n");
		random_char(o, (int)TOX_FILE_ID_LENGTH);

		filenum = tox_file_send(m, num, TOX_FILE_KIND_DATA, (uint64_t)filesize, (uint8_t*)o,
			(uint8_t *)file_name, namelen, &err);
	}
	else
	{
		dbg(9, "resume == 1\n");

		filenum = tox_file_send(m, num, TOX_FILE_KIND_DATA, (uint64_t)filesize, fileid_resume,
			(uint8_t *)file_name, namelen, &err);
	}
	dbg(2, "send_file_to_friend:tox_file_send=%s filenum=%d\n", file_name, (int)filenum);

    if (err != TOX_ERR_FILE_SEND_OK)
    {
		dbg(0, "send_file_to_friend_real: ! TOX_ERR_FILE_SEND_OK\n");
		goto on_send_error;
    }

	dbg(2, "send_file_to_friend_real(1):tox_file_send=%s filenum=%d\n", file_name, (int)filenum);
    struct FileTransfer *ft = new_file_transfer(num, filenum, FILE_TRANSFER_SEND, TOX_FILE_KIND_DATA);
	dbg(2, "send_file_to_friend_real(2):tox_file_send=%s filenum=%d\n", file_name, (int)filenum);

    if (!ft)
    {
		dbg(0, "send_file_to_friend_real:ft=NULL\n");
		err = TOX_ERR_FILE_SEND_TOO_MANY;
		goto on_send_error;
    }

    memcpy(ft->file_name, file_name, namelen + 1);
    ft->file = file_to_send;
    ft->file_size = filesize;

	if (resume == 0)
	{
		dbg(9, "resume == 0\n");
		memcpy(ft->file_id, o, (size_t)TOX_FILE_ID_LENGTH);
	}
	else
	{
		dbg(9, "resume == 1\n");
		memcpy(ft->file_id, fileid_resume, (size_t)TOX_FILE_ID_LENGTH);
	}

	dbg(0, "send_file_to_friend_real:tox_file_get_file_id num=%d filenum=%d\n", (int)num, (int)filenum);
	// dbg(0, "send_file_to_friend_real:file_id_resume=%d ft->file_id=%d\n", (int)fileid_resume, (int)ft->file_id);
	// dbg(0, "send_file_to_friend_real:o=%d ft->file_id=%d\n", (int)o, (int)ft->file_id);

	char file_id_str[TOX_FILE_ID_LENGTH * 2 + 1];
	bin_id_to_string_all((char*)ft->file_id, (size_t)TOX_FILE_ID_LENGTH, file_id_str, (size_t)(TOX_FILE_ID_LENGTH * 2 + 1));
	dbg(2, "send_file_to_friend_real:file_id=%s\n", file_id_str);
	bin_id_to_string_all((char*)fileid_resume, (size_t)TOX_FILE_ID_LENGTH, file_id_str, (size_t)(TOX_FILE_ID_LENGTH * 2 + 1));
	dbg(2, "send_file_to_friend_real:fileid_resume=%s\n", file_id_str);
	bin_id_to_string_all((char*)o, (size_t)TOX_FILE_ID_LENGTH, file_id_str, (size_t)(TOX_FILE_ID_LENGTH * 2 + 1));
	dbg(2, "send_file_to_friend_real:o=%s\n", file_id_str);

	free(o);
	o = NULL;

    return;

on_send_error:

	free(o);
	o = NULL;

    switch (err)
	{
        case TOX_ERR_FILE_SEND_FRIEND_NOT_FOUND:
            errmsg = "File transfer failed: Invalid friend.";
            break;

        case TOX_ERR_FILE_SEND_FRIEND_NOT_CONNECTED:
            errmsg = "File transfer failed: Friend is offline.";

            break;

        case TOX_ERR_FILE_SEND_NAME_TOO_LONG:
            errmsg = "File transfer failed: Filename is too long.";
            break;

        case TOX_ERR_FILE_SEND_TOO_MANY:
            errmsg = "File transfer failed: Too many concurrent file transfers.";

            break;

        default:
            errmsg = "File transfer failed.";
            break;
    }

    dbg(0, "send_file_to_friend_real:ft error=%s\n", errmsg);
    tox_file_control(m, num, filenum, TOX_FILE_CONTROL_CANCEL, NULL);
    fclose(file_to_send);

    // ------- hack to send file --------
    // ------- hack to send file --------
}

void resume_file_to_friend(Tox *m, uint32_t num, struct FileTransfer* ft)
{
	char *file_name_incl_full_path = NULL;
	int j = find_friend_in_friendlist(ft->friendnum);

	if (j > -1)
	{
		file_name_incl_full_path = malloc(300);
		snprintf(file_name_incl_full_path, 299, "%s/%s", (const char*)Friends.list[j].worksubdir, ft->file_name);
		dbg(2, "resume_file_to_friend:full path=%s\n", file_name_incl_full_path);
		char file_id_str[TOX_FILE_ID_LENGTH * 2 + 1];
		bin_id_to_string_all((char*)ft->file_id, (size_t)TOX_FILE_ID_LENGTH, file_id_str, (size_t)(TOX_FILE_ID_LENGTH * 2 + 1));
		// dbg(2, "resume_file_to_friend:file_id=%d file_id_bin=%d\n", (int)file_id_str, (int)ft->file_id);
		dbg(2, "resume_file_to_friend:file_id=%s\n", file_id_str);

		dbg(2, "resume_file_to_friend:path=%s friendnum=%d filenum=%d\n", file_name_incl_full_path, (int)ft->friendnum, (int)ft->filenum);
		send_file_to_friend_real(m, ft->friendnum, file_name_incl_full_path, 1, ft->file_id);
	}
	else
	{
		dbg(0, "resume_file_to_friend:friend %d not found in friendlist\n", (int)ft->friendnum);
	}
}

void send_file_to_friend(Tox *m, uint32_t num, const char* filename)
{
	send_file_to_friend_real(m, num, filename, 0, NULL);
}


int copy_file(const char *from, const char *to)
{
    int fd_to, fd_from;
    char buf[4096];
    ssize_t nread;
    int saved_errno;

    fd_from = open(from, O_RDONLY);

    if (fd_from < 0)
	{
		dbg(0, "copy_file:002\n");
        return -1;
	}

    fd_to = open(to, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd_to < 0)
	{
		dbg(0, "copy_file:003\n");
        goto out_error;
	}

    while (nread = read(fd_from, buf, sizeof buf), nread > 0)
    {
        char *out_ptr = buf;
        ssize_t nwritten;

        do
		{
            nwritten = write(fd_to, out_ptr, nread);

            if (nwritten >= 0)
            {
                nread -= nwritten;
                out_ptr += nwritten;
            }
            else if (errno != EINTR)
            {
				dbg(0, "copy_file:004\n");
                goto out_error;
            }

        } while (nread > 0);
    }

    if (nread == 0)
    {
        if (close(fd_to) < 0)
        {
            fd_to = -1;
            dbg(0, "copy_file:005\n");
            goto out_error;
        }

        close(fd_from);

        /* Success! */
        return 0;
    }


  out_error:
    saved_errno = errno;

    close(fd_from);
    if (fd_to >= 0)
	{
        close(fd_to);
	}

	dbg(0, "copy_file:009\n");

    errno = saved_errno;
    return -1;
}



char* copy_file_to_friend_subdir(int friendlistnum, const char* file_with_path, const char* filename)
{
}

int have_resumed_fts_friend(uint32_t friendnum)
{
	int j = find_friend_in_friendlist(friendnum);

	if (Friends.list[j].have_resumed_fts == 1)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

void send_file_to_all_friends(Tox *m, const char* file_with_path, const char* filename)
{
}

void on_tox_friend_status(Tox *tox, uint32_t friend_number, TOX_USER_STATUS status, void *user_data)
{
	dbg(2, "on_tox_friend_status:friendnum=%d status=%d\n", (int)friend_number, (int)status);
}

void friendlist_onConnectionChange(Tox *m, uint32_t num, TOX_CONNECTION connection_status, void *user_data)
{
	int friendlistnum = find_friend_in_friendlist(num);
    dbg(2, "friendlist_onConnectionChange:*ENTER*:friendnum=%d %d\n", (int)num, (int)connection_status);

    Friends.list[friendlistnum].connection_status = connection_status;
	update_friend_last_online(num, get_unix_time());

	if (is_friend_online(m, num) == 1)
	{
		dbg(0, "friend %d just got online\n", num);

		resume_resumable_fts(m, num);

		if (avatar_send(m, num) == -1)
		{
			dbg(0, "avatar_send failed for friend %d\n", num);
		}
	}
	else
	{
		dbg(0, "friend %d went *OFFLINE*\n", num);

		// save all resumeable FTs
		save_resumable_fts(m, num);
		// friend went offline -> cancel all filetransfers
		kill_all_file_transfers_friend(m, num);
		// friend went offline -> hang up on all calls
		av_local_disconnect(mytox_av, num);
	}

    dbg(2, "friendlist_onConnectionChange:*READY*:friendnum=%d %d\n", (int)num, (int)connection_status);

}

void friendlist_onFriendAdded(Tox *m, uint32_t num, bool sort)
{
    // dbg(9, "friendlist_onFriendAdded:001\n");

    if (Friends.max_idx == 0)
    {
		// dbg(9, "friendlist_onFriendAdded:001.a malloc 1 friend struct, max_id=%d, num=%d\n", (int)Friends.max_idx, (int)num);
        Friends.list = malloc(sizeof(ToxicFriend));
    }
    else
    {
		// dbg(9, "friendlist_onFriendAdded:001.b realloc %d friend struct, max_id=%d, num=%d\n", (int)(Friends.max_idx + 1), (int)Friends.max_idx, (int)num);
        Friends.list = realloc(Friends.list, ((Friends.max_idx + 1) * sizeof(ToxicFriend)));
    }

	// dbg(9, "friendlist_onFriendAdded:001.c set friend to all 0 values\n");
    memset(&Friends.list[Friends.max_idx], 0, sizeof(ToxicFriend)); // fill friend with "0" bytes


	// dbg(2, "friendlist_onFriendAdded:003:%d\n", (int)Friends.max_idx);
	Friends.list[Friends.max_idx].num = num;
	Friends.list[Friends.max_idx].active = true;
	Friends.list[Friends.max_idx].connection_status = TOX_CONNECTION_NONE;
	Friends.list[Friends.max_idx].status = TOX_USER_STATUS_NONE;
	Friends.list[Friends.max_idx].waiting_for_answer = 0;
	Friends.list[Friends.max_idx].auto_resend_start_time = 0;
	Friends.list[Friends.max_idx].have_resumed_fts = 0;

	TOX_ERR_FRIEND_GET_PUBLIC_KEY pkerr;
	tox_friend_get_public_key(m, num, (uint8_t *) Friends.list[Friends.max_idx].pub_key, &pkerr);

	if (pkerr != TOX_ERR_FRIEND_GET_PUBLIC_KEY_OK)
	{
		dbg(0, "tox_friend_get_public_key failed (error %d)\n", pkerr);
	}

	bin_id_to_string(Friends.list[Friends.max_idx].pub_key, (size_t) TOX_ADDRESS_SIZE, Friends.list[Friends.max_idx].pubkey_string, (size_t) (TOX_ADDRESS_SIZE * 2 + 1));
	// dbg(2, "friend pubkey=%s\n", Friends.list[Friends.max_idx].pubkey_string);

	TOX_ERR_FRIEND_GET_LAST_ONLINE loerr;
	time_t t = tox_friend_get_last_online(m, num, &loerr);

	if (loerr != TOX_ERR_FRIEND_GET_LAST_ONLINE_OK)
	{
	    t = 0;
	}

	update_friend_last_online(num, t);

	Friends.max_idx++;

}

// after you are finished call free_friendlist_nums !
uint32_t* load_friendlist_nums(Tox *m)
{
	size_t numfriends = tox_self_get_friend_list_size(m);
	uint32_t *friend_list = malloc(numfriends * sizeof(uint32_t));
	tox_self_get_friend_list(m, friend_list);

	return friend_list;
}

void free_friendlist_nums(void* friend_list)
{
	if (friend_list)
	{
		free(friend_list);
		friend_list = NULL;
	}
}

static void load_friendlist(Tox *m)
{
    size_t i;
	// TODO
    size_t numfriends = tox_self_get_friend_list_size(m);
	uint32_t* friend_lookup_list = load_friendlist_nums(m);

    for (i = 0; i < numfriends; ++i)
    {
        friendlist_onFriendAdded(m, friend_lookup_list[i], false);
        dbg(2, "loading friend num:%d pubkey=%s\n", (int)friend_lookup_list[i], Friends.list[Friends.max_idx - 1].pubkey_string);
    }

	free_friendlist_nums((void*) friend_lookup_list);
}




void close_file_transfer(Tox *m, struct FileTransfer *ft, int CTRL)
{
    dbg(9, "close_file_transfer:001\n");

    if (!ft)
	{
        return;
	}

    if (ft->state == FILE_TRANSFER_INACTIVE)
	{
        return;
	}

    if (ft->file)
	{
        fclose(ft->file);
	}

    if (CTRL >= 0)
	{
        tox_file_control(m, ft->friendnum, ft->filenum, (TOX_FILE_CONTROL) CTRL, NULL);
	}

    memset(ft, 0, sizeof(struct FileTransfer));
	ft->state = FILE_TRANSFER_INACTIVE; // == 0

}

int has_reached_max_file_transfer_for_friend(uint32_t num)
{
	int active_ft = 0;
	int friendlistnum = find_friend_in_friendlist(num);
	int i;

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft_send = &Friends.list[friendlistnum].file_sender[i];

        if (ft_send->state != FILE_TRANSFER_INACTIVE)
        {
			if (ft_send->file_name != NULL)
			{
				active_ft++;
			}
		}
	}

	if (active_ft < MAX_FILES)
	{
		return 0;
	}
	else
	{
		// have reached max filetransfers already
		return 1;
	}
}

struct FileTransfer *get_file_transfer_from_filename_struct(int friendlistnum, const char* filename)
{
    size_t i;

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft_send = &Friends.list[friendlistnum].file_sender[i];

        if (ft_send->state != FILE_TRANSFER_INACTIVE)
        {
			if (ft_send->file_name != NULL)
			{
				if ((strlen(ft_send->file_name) > 0) && (filename != NULL) && (strlen(filename) > 0))
				{
					if (strncmp((char*)ft_send->file_name, filename, strlen(ft_send->file_name)) == 0)
					{
						// dbg(9, "found ft by filename:%s\n", ft_send->file_name);
						return ft_send;
					}
				}
			}
        }
    }

    return NULL;
}


struct FileTransfer *get_file_transfer_struct(uint32_t friendnum, uint32_t filenum)
{
    size_t i;

	int friendlistnum = find_friend_in_friendlist(friendnum);

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft_send = &Friends.list[friendlistnum].file_sender[i];

        if (ft_send->state != FILE_TRANSFER_INACTIVE && ft_send->filenum == filenum)
        {
            return ft_send;
        }

        struct FileTransfer *ft_recv = &Friends.list[friendlistnum].file_receiver[i];

        if (ft_recv->state != FILE_TRANSFER_INACTIVE && ft_recv->filenum == filenum)
        {
            return ft_recv;
        }
    }

    return NULL;
}

//
// cut message at 999 chars length !!
//
void send_text_message_to_friend(Tox *tox, uint32_t friend_number, const char *fmt, ...)
{
	char msg2[1000];
	size_t length = 0;

	if (fmt == NULL)
	{
		dbg(9, "send_text_message_to_friend:no message to send\n");
		return;
	}

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg2, 999, fmt, ap);
	va_end(ap);

	length = (size_t)strlen(msg2);
	tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)msg2, length, NULL);
}


void friend_request_cb(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length,
                                   void *user_data)
{
    uint32_t friendnum = tox_friend_add_norequest(tox, public_key, NULL);
    dbg(2, "add friend:002:friendnum=%d max_id=%d\n", friendnum, (int)Friends.max_idx);
    friendlist_onFriendAdded(tox, friendnum, 0);

    update_savedata_file(tox);

	tox_self_set_nospam(tox, generate_random_uint32());
    update_savedata_file(tox);

	// new nospam created -> generate new QR code image
    print_tox_id(tox);
	delete_qrcode_generate_touchfile();
	create_tox_id_qrcode(tox);
}

/* ssssshhh I stole this from ToxBot, don't tell anyone.. */
/* ssssshhh and I stole this from EchoBot, don't tell anyone.. */
static void get_elapsed_time_str(char *buf, int bufsize, uint64_t secs)
{
	long unsigned int minutes = (secs % 3600) / 60;
	long unsigned int hours = (secs / 3600) % 24;
	long unsigned int days = (secs / 3600) / 24;

	snprintf(buf, bufsize, "%lud %luh %lum", days, hours, minutes);
}

//
// lastline param ignored for now!!
//
void run_cmd_return_output(const char *command, char* output, int lastline)
{
	FILE *fp = NULL;
	char path[1035];
	char *pos = NULL;

	if (!output)
	{
		return;
	}

	/* Open the command for reading. */
	fp = popen(command, "r");
	if (fp == NULL)
	{
		dbg(0, "Failed to run command: %s errno=%d error=%s\n", command, errno, strerror(errno));
		output[0] = '\0';
		return;
	}

	/* Read the output a line at a time - output it. */
	while (fgets(path, sizeof(path)-1, fp) != NULL)
	{
        snprintf(output, 299, "%s", (const char*)path);
	}

	if (strlen(output) > 1)
	{
		if ((pos = strchr(output, '\n')) != NULL)
		{
			*pos = '\0';
		}
	}

	/* close */
	pclose(fp);
}

void remove_friend(Tox *tox, uint32_t friend_number)
{
	TOX_ERR_FRIEND_DELETE error;
	tox_friend_delete(tox, friend_number, &error);
}

void cmd_delfriend(Tox *tox, uint32_t friend_number, const char* message)
{
	uint32_t del_friend_number = -1;
	if (friend_number != del_friend_number)
	{
		// remove_friend(tox, del_friend_number);
	}
}

void cmd_stats(Tox *tox, uint32_t friend_number)
{
	switch (my_connection_status)
	{
		case TOX_CONNECTION_NONE:
			send_text_message_to_friend(tox, friend_number, "toxblinkenwall status:offline");
			break;
		case TOX_CONNECTION_TCP:
			send_text_message_to_friend(tox, friend_number, "toxblinkenwall status:Online, using TCP");
			break;
		case TOX_CONNECTION_UDP:
			send_text_message_to_friend(tox, friend_number, "toxblinkenwall status:Online, using UDP");
			break;
		default:
			send_text_message_to_friend(tox, friend_number, "toxblinkenwall status:*unknown*");
			break;
	}

	// ----- uptime -----
	char time_str[200];
	uint64_t cur_time = time(NULL);
	get_elapsed_time_str(time_str, sizeof(time_str), cur_time - global_start_time);
	send_text_message_to_friend(tox, friend_number, "Uptime: %s", time_str);
	// ----- uptime -----

	char output_str[1000];
	run_cmd_return_output(shell_cmd__get_my_number_of_open_files, output_str, 1);
	if (strlen(output_str) > 0)
	{
		send_text_message_to_friend(tox, friend_number, "toxblinkenwall open files:%s", output_str);
	}
	else
	{
		send_text_message_to_friend(tox, friend_number, "ERROR getting open files");
	}

	// --- temp ---
	run_cmd_return_output(shell_cmd__get_cpu_temp, output_str, 1);
	if (strlen(output_str) > 0)
	{
		send_text_message_to_friend(tox, friend_number, "toxblinkenwall Cpu temp:%s\xC2\xB0%s", output_str, "C");
	}
	else
	{
		send_text_message_to_friend(tox, friend_number, "ERROR getting Cpu temp");
	}

	run_cmd_return_output(shell_cmd__get_gpu_temp, output_str, 1);
	if (strlen(output_str) > 0)
	{
		send_text_message_to_friend(tox, friend_number, "toxblinkenwall GPU temp:%s\xC2\xB0%s", output_str, "C");
	}
	else
	{
		send_text_message_to_friend(tox, friend_number, "ERROR getting GPU temp");
	}
	// --- temp ---

    // ----- bit rates -----
    send_text_message_to_friend(tox, friend_number, "Bitrates (kb/s): audio=%d video=%d",
                                (int)global_audio_bit_rate, (int)global_video_bit_rate);
    // ----- bit rates -----

    send_text_message_to_friend(tox, friend_number, "show every n-th video frame: value=%d",
                                (int)SHOW_EVERY_X_TH_VIDEO_FRAME);


    send_text_message_to_friend(tox, friend_number, "sleep in ms between video frames: value=%d",
                                (int)DEFAULT_FPS_SLEEP_MS);

    send_text_message_to_friend(tox, friend_number, "setting vpx kf_max_dist: value=%d",
                                (int)global__VPX_KF_MAX_DIST);

    send_text_message_to_friend(tox, friend_number, "setting vpx g_lag_in_frames: value=%d",
                                (int)global__VPX_G_LAG_IN_FRAMES);


	if (global__VPX_END_USAGE == 0)
	{
		send_text_message_to_friend(tox, friend_number, "setting vpx end usage: VPX_VBR Variable Bit Rate (VBR) mode");
	}
	else if (global__VPX_END_USAGE == 1)
	{
		send_text_message_to_friend(tox, friend_number, "setting vpx end usage: VPX_CBR Constant Bit Rate (CBR) mode");
	}
	else if (global__VPX_END_USAGE == 2)
	{
		send_text_message_to_friend(tox, friend_number, "setting vpx end usage: VPX_CQ  *Constrained Quality (CQ)  mode");
	}
	else if (global__VPX_END_USAGE == 3)
	{
		send_text_message_to_friend(tox, friend_number, "setting vpx end usage: VPX_Q   Constant Quality (Q) mode");
	}

    send_text_message_to_friend(tox, friend_number, "VPX CPU_USED value [-16 .. 16]: value=%d",
                                (int)global__VP8E_SET_CPUUSED_VALUE);


    char tox_id_hex[TOX_ADDRESS_SIZE*2 + 1];
	get_my_toxid(tox, tox_id_hex);

	send_text_message_to_friend(tox, friend_number, "tox:%s", tox_id_hex);
}

void cmd_kamft(Tox *tox, uint32_t friend_number)
{
	send_text_message_to_friend(tox, friend_number, "killing all filetransfers to you ...");
	kill_all_file_transfers_friend(tox, friend_number);
}

void cmd_snap(Tox *tox, uint32_t friend_number)
{
	send_text_message_to_friend(tox, friend_number, "*Feature DISABLED*");

	if (1 == 1 + 1)
	{
		send_text_message_to_friend(tox, friend_number, "capture single shot, and send to all friends ...");

		char output_str[1000];
		run_cmd_return_output(shell_cmd__single_shot, output_str, 1);

#if 0
		if (strlen(output_str) > 0)
		{
			// send_text_message_to_friend(tox, friend_number, "toxblinkenwall:%s", output_str);
		}
		else
		{
			send_text_message_to_friend(tox, friend_number, "ERROR running snap command");
		}
#endif

		send_text_message_to_friend(tox, friend_number, "... capture single shot, ready!");

	}
}

void cmd_friends(Tox *tox, uint32_t friend_number)
{
	size_t i;
	// TODO
    size_t numfriends = tox_self_get_friend_list_size(tox);
	int j = -1;

    for (i = 0; i < numfriends; ++i)
    {
		j = find_friend_in_friendlist((uint32_t) i);
		if (j > -1)
		{
			send_text_message_to_friend(tox, friend_number, "%d:friend", j);
			send_text_message_to_friend(tox, friend_number, "%d tox:%s", j, (const char*)Friends.list[j].pubkey_string);
			send_text_message_to_friend(tox, friend_number, "%d:last online (in client local time):%s", j, (const char*)Friends.list[j].last_online.hour_min_str);

			switch (Friends.list[j].connection_status)
			{
				case TOX_CONNECTION_NONE:
					send_text_message_to_friend(tox, friend_number, "%d:%s", j, "status:offline");
					break;
				case TOX_CONNECTION_TCP:
					send_text_message_to_friend(tox, friend_number, "%d:%s", j, "status:Online, using TCP");
					break;
				case TOX_CONNECTION_UDP:
					send_text_message_to_friend(tox, friend_number, "%d:%s", j, "status:Online, using UDP");
					break;
				default:
					send_text_message_to_friend(tox, friend_number, "%d:%s", j, "status:*unknown*");
					break;
			}
		}
    }
}

void cmd_restart(Tox *tox, uint32_t friend_number)
{
	send_text_message_to_friend(tox, friend_number, "toxblinkenwall services will restart ...");

	global_want_restart = 1;
}


void cmd_vcm(Tox *tox, uint32_t friend_number)
{
	// send_text_message_to_friend(tox, friend_number, "video-call-me not yet implemented!");

	dbg(9, "cmd_vcm:001\n");

	if (global_video_active == 1)
	{
		send_text_message_to_friend(tox, friend_number, "there is already a video session active");
	}
	else
	{
		send_text_message_to_friend(tox, friend_number, "i am trying to send my video ...");

		if (mytox_av != NULL)
		{
			dbg(9, "cmd_vcm:003\n");
			global_video_bit_rate = DEFAULT_GLOBAL_VID_BITRATE;
			friend_to_send_video_to = friend_number;
			dbg(9, "cmd_vcm:004\n");

			update_status_line_1_text();

			TOXAV_ERR_CALL error = 0;
			toxav_call(mytox_av, friend_number, global_audio_bit_rate, global_video_bit_rate, &error);
			// toxav_call(mytox_av, friend_number, 0, 40, &error);
			dbg(9, "cmd_vcm:005\n");

			if (error != TOXAV_ERR_CALL_OK)
			{
				switch (error)
				{
					case TOXAV_ERR_CALL_MALLOC:
						dbg(0, "toxav_call (1):TOXAV_ERR_CALL_MALLOC\n");
						break;

					case TOXAV_ERR_CALL_SYNC:
						dbg(0, "toxav_call (1):TOXAV_ERR_CALL_SYNC\n");
						break;

					case TOXAV_ERR_CALL_FRIEND_NOT_FOUND:
						dbg(0, "toxav_call (1):TOXAV_ERR_CALL_FRIEND_NOT_FOUND\n");
						break;

					case TOXAV_ERR_CALL_FRIEND_NOT_CONNECTED:
						dbg(0, "toxav_call (1):TOXAV_ERR_CALL_FRIEND_NOT_CONNECTED\n");
						break;

					case TOXAV_ERR_CALL_FRIEND_ALREADY_IN_CALL:
						dbg(0, "toxav_call (1):TOXAV_ERR_CALL_FRIEND_ALREADY_IN_CALL\n");
						break;

					case TOXAV_ERR_CALL_INVALID_BIT_RATE:
						dbg(0, "toxav_call (1):TOXAV_ERR_CALL_INVALID_BIT_RATE\n");
						break;

					default:
						dbg(0, "toxav_call (1):*unknown error*\n");
						break;
				}
			}
		}
		else
		{
			dbg(9, "cmd_vcm:006\n");
			send_text_message_to_friend(tox, friend_number, "sending video failed:toxav==NULL");
		}
	}

	dbg(9, "cmd_vcm:099\n");
}

void send_help_to_friend(Tox *tox, uint32_t friend_number)
{
	send_text_message_to_friend(tox, friend_number, "=========================\nToxBlinkenwall version:%s\n=========================", global_version_string);
	send_text_message_to_friend(tox, friend_number, " .stats         --> show ToxBlinkenwall status");
	send_text_message_to_friend(tox, friend_number, " .friends       --> show ToxBlinkenwall Friends");
	send_text_message_to_friend(tox, friend_number, " .showclients   --> show Clientapp links");
	send_text_message_to_friend(tox, friend_number, " .showqr        --> show ToxID");
	send_text_message_to_friend(tox, friend_number, " .text <text>   --> show Text on Wall");
	send_text_message_to_friend(tox, friend_number, " .restart       --> restart ToxBlinkenwall system");
	send_text_message_to_friend(tox, friend_number, " .vcm           --> videocall me");
	send_text_message_to_friend(tox, friend_number, " .kac           --> Kill all calls");
    send_text_message_to_friend(tox, friend_number, " .vbr <v rate>  --> set video bitrate to <v rate> kbits/s");
    send_text_message_to_friend(tox, friend_number, " .skpf <num>    --> show only every n-th video frame");
    send_text_message_to_friend(tox, friend_number, " .showfps       --> show fps on video");
    send_text_message_to_friend(tox, friend_number, " .hidefps       --> hide fps on video");
    send_text_message_to_friend(tox, friend_number, " .cpu <vpx cpu used> --> set VPX CPU_USED: -16 .. 16");
    send_text_message_to_friend(tox, friend_number, " .kfmax <vpx KF max> -->");
    send_text_message_to_friend(tox, friend_number, " .glag <vpx lag fr>  -->");
    send_text_message_to_friend(tox, friend_number, " .vpxu <end usage>   --> set VPX END_USAGE");
    send_text_message_to_friend(tox, friend_number, " .fps <delay ms>     --> set delay in ms between sent frames");
    send_text_message_to_friend(tox, friend_number, " .set <num> <ToxID>  --> set <ToxId> as book entry <num>");
    send_text_message_to_friend(tox, friend_number, " .del <num>          --> remove book entry <num>");
    send_text_message_to_friend(tox, friend_number, " .call <num>         --> call book entry <num>");
}

//void start_zipfile(mz_zip_archive *pZip, size_t size_pZip, const char* zip_file_full_path)
//{
//}
//void add_file_to_zipfile(mz_zip_archive *pZip, const char* file_to_add_full_path, const char* filename_in_zipfile)
//{
//}
//void finish_zipfile(mz_zip_archive *pZip)
//{
//}

void invite_toxid_as_friend(Tox *tox, uint8_t *tox_id_bin)
{
    if (tox_id_bin == NULL)
    {
        dbg(9, "no ToxID\n");
        return;
    }

    int64_t fnum = friend_number_for_entry(tox, tox_id_bin);
    if (fnum == -1)
    {
        dbg(9, "ToxID not on friendlist, inviting ...\n");
        const char *message_str = "invite ...";
        TOX_ERR_FRIEND_ADD error;
        uint32_t friendnum = tox_friend_add(tox, (uint8_t *) tox_id_bin,
                                            (uint8_t *) message_str,
                                            (size_t) strlen(message_str),
                                            &error);

        if (error != 0)
        {
            if (error == TOX_ERR_FRIEND_ADD_ALREADY_SENT)
            {
                dbg(9, "add friend:ERROR:TOX_ERR_FRIEND_ADD_ALREADY_SENT\n");
            }
            else
            {
                dbg(9, "add friend:ERROR:%d\n", (int) error);
            }
        }
        else
        {
            dbg(9, "friend request sent.\n");
        }
    }
    else
    {
        dbg(9, "ToxID already a friend\n");
    }

    update_savedata_file(tox);
}

bool file_exists(const char *path)
{
    struct stat s;
    return stat(path, &s) == 0;
}

void create_entry_file_if_not_exists(int entry_num)
{
	char entry_toxid_filename[300];
	snprintf(entry_toxid_filename, 299, "book_entry_%d.txt", entry_num);

    if (!file_exists(entry_toxid_filename))
    {
        FILE *fp = fopen(entry_toxid_filename, "w");

        if (fp == NULL)
        {
            dbg(1, "Warning: failed to create %s file\n", entry_toxid_filename);
            return;
        }

        fclose(fp);
        dbg(1, "Warning: creating new %s file. Did you lose the old one?\n", entry_toxid_filename);
    }
}

void read_pubkey_from_file(uint8_t **toxid_str, int entry_num)
{
    create_entry_file_if_not_exists(entry_num);
    *toxid_str = NULL;

	char entry_toxid_filename[300];
	snprintf(entry_toxid_filename, 299, "book_entry_%d.txt", entry_num);

    FILE *fp = fopen(entry_toxid_filename, "r");
    if (fp == NULL)
    {
        dbg(1, "Warning: failed to read %s file\n", entry_toxid_filename);
        return;
    }

    char id[256];
    int len;
    while (fgets(id, sizeof(id), fp))
    {
        len = strlen(id);
        if (len < (TOX_ADDRESS_SIZE * 2))
        {
            continue;
        }

        *toxid_str = hex_string_to_bin(id);
        break;
    }

    fclose(fp);
}

void delete_entry_file(int entry_num)
{
	if ((entry_num >= 1) && (entry_num <= 9))
	{
		char entry_toxid_filename[300];
		snprintf(entry_toxid_filename, 299, "book_entry_%d.txt", entry_num);

		unlink(entry_toxid_filename);
	}
}

void write_pubkey_to_entry_file(uint8_t *toxid_bin, int entry_num)
{
    if (toxid_bin == NULL)
    {
        delete_entry_file(entry_num);
    }
    else
    {
        create_entry_file_if_not_exists(entry_num);

        char entry_toxid_filename[300];
        snprintf(entry_toxid_filename, 299, "book_entry_%d.txt", entry_num);

        FILE *fp = fopen(entry_toxid_filename, "wb");
        if (fp == NULL)
        {
            dbg(1, "Warning: failed to read %s file\n", entry_toxid_filename);
            return;
        }

        char toxid_pubkey_string[TOX_ADDRESS_SIZE * 2 + 1];
        CLEAR(toxid_pubkey_string);
        bin_to_hex_string(toxid_bin, (size_t) TOX_ADDRESS_SIZE, toxid_pubkey_string);

        int result = fputs(toxid_pubkey_string, fp);
        fclose(fp);
    }
}

void disconnect_all_calls(Tox *tox)
{
    size_t i = 0;
    size_t size = tox_self_get_friend_list_size(tox);

    if (size == 0)
    {
        return;
    }

    uint32_t list[size];
    tox_self_get_friend_list(tox, list);
    char friend_key[TOX_PUBLIC_KEY_SIZE];
    CLEAR(friend_key);

    for (i = 0; i < size; ++i)
    {
        av_local_disconnect(mytox_av, list[i]);
    }
}

void update_status_line_1_text()
{
	snprintf(status_line_1_str, sizeof(status_line_1_str), "V: I/O/OB %d/%d/%d", (int)global_video_in_fps, (int)global_video_out_fps, (int)global_video_bit_rate);
	// dbg(9, "update_status_line_1_text:global_video_bit_rate=%d\n", (int)global_video_bit_rate);
}

void update_status_line_1_text_arg(int vbr_)
{
	snprintf(status_line_1_str, sizeof(status_line_1_str), "V: I/O/OB %d/%d/%d", (int)global_video_in_fps, (int)global_video_out_fps, vbr_);
}

void update_status_line_2_text()
{
	if (speaker_out_num == 0)
	{
		snprintf(status_line_2_str, sizeof(status_line_2_str),    "A: %s OB %d", speaker_out_name_0, (int)global_audio_bit_rate);
	}
	else
	{
		snprintf(status_line_2_str, sizeof(status_line_2_str),    "A: %s OB %d", speaker_out_name_1, (int)global_audio_bit_rate);
	}
}

void friend_message_cb(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                   size_t length, void *user_data)
{
	int j;
	int send_back = 0;

    if (type == TOX_MESSAGE_TYPE_NORMAL)
    {
		if (message != NULL)
		{
			j = find_friend_in_friendlist(friend_number);
			dbg(2, "message from friend:%d msg:%s\n", (int)friend_number, (char*)message);

			if (strncmp((char*)message, ".help", strlen((char*)".help")) == 0)
			{
				send_help_to_friend(tox, friend_number);
			}
			else if (strncmp((char*)message, "help", strlen((char*)"help")) == 0)
			{
				send_help_to_friend(tox, friend_number);
			}
			else if (strncmp((char*)message, ".stats", strlen((char*)".stats")) == 0)
			{
				cmd_stats(tox, friend_number);
			}
			else if (strncmp((char*)message, ".friends", strlen((char*)".friends")) == 0)
			{
				cmd_friends(tox, friend_number);
			}
			else if (strncmp((char*)message, ".showclients", strlen((char*)".showclients")) == 0)
			{
				global_is_qrcode_showing_on_screen = 0;
				show_tox_client_application_download_links();
			}
			else if (strncmp((char*)message, ".showqr", strlen((char*)".showqr")) == 0)
			{
				if (global_video_active == 0)
				{
					show_tox_id_qrcode();
				}
			}
			else if (strncmp((char*)message, ".text", strlen((char*)".text")) == 0)
			{
				if (accepting_calls == 1)
				{
					global_is_qrcode_showing_on_screen = 0;
					show_text_as_image(message);
				}
			}
			//else if (strncmp((char*)message, ".snap", strlen((char*)".snap")) == 0)
			//{
			//	cmd_snap(tox, friend_number);
			//}
			else if (strncmp((char*)message, ".restart", strlen((char*)".restart")) == 0) // restart toxblinkenwall processes (no reboot)
			{
				global_is_qrcode_showing_on_screen = 0;
				cmd_restart(tox, friend_number);
			}
			else if (strncmp((char*)message, ".vcm", strlen((char*)".vcm")) == 0) // video call me!
			{
				if (accepting_calls == 1)
				{
					cmd_vcm(tox, friend_number);
				}
			}
			else if (strncmp((char *)message, ".kac", strlen((char *) ".kac")) == 0)
			{
				disconnect_all_calls(tox);
			}
			else if (strncmp((char*)message, ".set ", strlen((char*)".set ")) == 0) // set book entry <num> to <ToxID> and add friend request!
			{
				if (strlen(message) == ((TOX_ADDRESS_SIZE * 2) + 7))
				{
					char *only_num = strndup((message + 5), 1);
					int entry_num = get_number_in_string(only_num, (int)-1);
					free(only_num);

					if ((entry_num >= 1) && (entry_num <=9))
					{
						const char *entry_hex_toxid_string = (message + 7);
						uint8_t *entry_bin_toxid = hex_string_to_bin(entry_hex_toxid_string);

						if (entry_bin_toxid)
						{
							write_pubkey_to_entry_file(entry_bin_toxid, entry_num);

							int64_t is_already_friendnum = friend_number_for_entry(tox, entry_bin_toxid);

							if (is_already_friendnum == -1)
							{
								invite_toxid_as_friend(tox, entry_bin_toxid);
							}

							free(entry_bin_toxid);
						}
					}
				}
			}
			else if (strncmp((char*)message, ".del ", strlen((char*)".del ")) == 0) // remove book entry <num>
			{
				if (strlen(message) == 6)
				{
					char *only_num = strndup((message + 5), 1);
					int entry_num = get_number_in_string(only_num, (int)-1);
					free(only_num);

					if ((entry_num >= 1) && (entry_num <=9))
					{
						delete_entry_file(entry_num);
					}
				}
			}
			else if (strncmp((char*)message, ".call ", strlen((char*)".call ")) == 0) // call book entry <num>
			{
				if (strlen(message) == 7) // 1 digit allowed only
				{
					int call_number_num = get_number_in_string(message, (int)-1);
					if ((call_number_num >= 1) && (call_number_num <= 9))
					{
						call_entry_num(tox, call_number_num);
					}
				}
			}
			else if (strncmp((char*)message, ".showfps", strlen((char*)".showfps ")) == 0) // show fps on video
			{
				global_show_fps_on_video = 1;
			}
			else if (strncmp((char*)message, ".hidefps", strlen((char*)".hidefps ")) == 0) // hide fps on video
			{
				global_show_fps_on_video = 0;
			}
			else if (strncmp((char*)message, ".skpf ", strlen((char*)".skpf ")) == 0) // set show every n-th video frame
			{
				if (strlen(message) > 6) // require 1 digits
				{
					int value_new = get_number_in_string(message, (int)SHOW_EVERY_X_TH_VIDEO_FRAME);
					if ((value_new >= 1) && (value_new <= 20))
					{
						SHOW_EVERY_X_TH_VIDEO_FRAME = value_new;
					}
				}
			}
			else if (strncmp((char*)message, ".fps ", strlen((char*)".fps ")) == 0) // set 1000/fps
			{
				if (strlen(message) > 5)
				{
					int num_new = get_number_in_string(message, (int)DEFAULT_FPS_SLEEP_MS);
					if ((num_new >= 1) && (num_new <= 1000))
					{
						DEFAULT_FPS_SLEEP_MS = num_new;
						dbg(9, "setting wait in ms: %d\n", (int)DEFAULT_FPS_SLEEP_MS);
					}
				}
			}			
			else if (strncmp((char*)message, ".vpxu ", strlen((char*)".vpxu ")) == 0) // set vpx end usage
			{
				if (strlen(message) > 6)
				{
					int num_new = get_number_in_string(message, (int)global__VPX_END_USAGE);
					if ((num_new >= 0) && (num_new <= 3))
					{
						global__VPX_END_USAGE = num_new;
						if (global__VPX_END_USAGE == 0)
						{
							dbg(9, "setting vpx end usage: VPX_VBR Variable Bit Rate (VBR) mode\n");
						}
						else if (global__VPX_END_USAGE == 1)
						{
							dbg(9, "setting vpx end usage: VPX_CBR Constant Bit Rate (CBR) mode\n");
						}
						else if (global__VPX_END_USAGE == 2)
						{
							dbg(9, "setting vpx end usage: VPX_CQ  *Constrained Quality (CQ)  mode\n");
						}
						else if (global__VPX_END_USAGE == 3)
						{
							dbg(9, "setting vpx end usage: VPX_Q   Constant Quality (Q) mode\n");
						}
					}
				}
			}			
			else if (strncmp((char*)message, ".vbr ", strlen((char*)".vbr ")) == 0) // set v bitrate
			{
				if (strlen(message) > 7) // require 3 digits
				{
					int vbr_new = get_number_in_string(message, (int)global_video_bit_rate);
					if ((vbr_new >= DEFAULT_GLOBAL_MIN_VID_BITRATE) && (vbr_new <= DEFAULT_GLOBAL_MAX_VID_BITRATE))
					{
						DEFAULT_GLOBAL_VID_BITRATE = (int32_t)vbr_new;
						global_video_bit_rate = DEFAULT_GLOBAL_VID_BITRATE;

						update_status_line_1_text();

						if (mytox_av != NULL)
						{
							toxav_bit_rate_set(mytox_av, friend_number, global_audio_bit_rate, global_video_bit_rate, NULL);
						}
					}
				}
			}
			else if (strncmp((char*)message, ".cpu ", strlen((char*)".cpu ")) == 0) // set CPU value for vp8 encoder
			{
				if (strlen(message) > 5)
				{
					int vbr_new = get_number_in_string(message, (int)global__VP8E_SET_CPUUSED_VALUE);
					if ((vbr_new >= -16) && (vbr_new <= 16))
					{
						if (mytox_av != NULL)
						{
							global__VP8E_SET_CPUUSED_VALUE = (int)vbr_new;
							toxav_bit_rate_set(mytox_av, friend_number, global_audio_bit_rate, global_video_bit_rate, NULL);
						}
					}
				}
			}
			else if (strncmp((char*)message, ".kfmax ", strlen((char*)".kfmax ")) == 0)
			{
				if (strlen(message) > 7)
				{
					int vbr_new = get_number_in_string(message, (int)global__VPX_KF_MAX_DIST);
					if ((vbr_new >= 1) && (vbr_new <= 200))
					{
						if (mytox_av != NULL)
						{
							global__VPX_KF_MAX_DIST = (int)vbr_new;
							toxav_bit_rate_set(mytox_av, friend_number, global_audio_bit_rate, global_video_bit_rate, NULL);
						}
					}
				}
			}
			else if (strncmp((char*)message, ".glag ", strlen((char*)".glag ")) == 0)
			{
				if (strlen(message) > 6)
				{
					int vbr_new = get_number_in_string(message, (int)global__VPX_G_LAG_IN_FRAMES);
					if ((vbr_new >= 0) && (vbr_new <= 100))
					{
						if (mytox_av != NULL)
						{
							global__VPX_G_LAG_IN_FRAMES = (int)vbr_new;
							toxav_bit_rate_set(mytox_av, friend_number, global_audio_bit_rate, global_video_bit_rate, NULL);
						}
					}
				}
			}
			else
			{
				if (Friends.list[j].waiting_for_answer == 1)
				{
					// we want to get user feedback
					snprintf(Friends.list[j].last_answer, 99, "%s", (char*)message);
					Friends.list[j].waiting_for_answer = 2;

					if (Friends.list[j].last_answer)
					{
						dbg(2, "got answer from friend:%d answer:%s\n", (int)friend_number, Friends.list[j].last_answer);
					}
					else
					{
						dbg(2, "got answer from friend:%d answer:NULL\n", (int)friend_number);
					}
				}
				else
				{
					// send_back = 1;
					// unknown command, just be quiet
				}
			}
		}
		else
		{
			dbg(2, "message from friend:%d msg:NULL\n", (int)friend_number);
		}
    }
    else
    {
		dbg(2, "message from friend:%d\n", (int)friend_number);
    }

	if (send_back == 1)
	{
		tox_friend_send_message(tox, friend_number, type, message, length, NULL);
	}
}



void on_file_recv_chunk(Tox *m, uint32_t friendnumber, uint32_t filenumber, uint64_t position,
                        const uint8_t *data, size_t length, void *user_data)
{
	if ((incoming_filetransfers_friendnumber == friendnumber) && (incoming_filetransfers_filenumber = filenumber))
	{

		// struct FileTransfer *ft = get_file_transfer_struct(friendnumber, filenumber);

		// if (!ft)
		// {
		//    return;
		//}

		if (length == 0)
		{
			// file transfer finished --------------
			// show image
			show_endless_image();
			incoming_filetransfers_friendnumber = -1;
			incoming_filetransfers_filenumber = -1;
			incoming_filetransfers = 0;
			// file transfer finished --------------
		}
		else
		{
			int fd_ = open(cmd__image_filename_full_path, O_WRONLY | O_CREAT, S_IRWXU);

			// dbg(9, "fd=%d\n", fd_);

			if (fd_ != -1)
			{
				FILE *some_file = fdopen(fd_, "wb");
				// dbg(9, "some_file=%p\n", some_file);

				if (some_file)
				{
					fseek(some_file, (long)position, SEEK_SET);
					fwrite(data, length, 1, some_file);
					fclose(some_file);
					// close(fd_);
				}
			}
		}
	}
}


void on_file_recv(Tox *m, uint32_t friendnumber, uint32_t filenumber, uint32_t kind, uint64_t file_size,
                  const uint8_t *filename, size_t filename_length, void *userdata)
{
    /* We don't care about receiving avatars */
    if (kind != TOX_FILE_KIND_DATA)
    {
        tox_file_control(m, friendnumber, filenumber, TOX_FILE_CONTROL_CANCEL, NULL);
        dbg(9, "on_file_recv:002:cancel incoming avatar\n");
        return;
    }
    else
    {
		if (incoming_filetransfers > 0)
		{
			// only ever 1 incoming filetransfer!
			tox_file_control(m, friendnumber, filenumber, TOX_FILE_CONTROL_CANCEL, NULL);
		}
		else
		{
			if (file_size < 10 * 1024 * 1024) // only accept files up to 10 Mbytes in size
			{
				stop_endless_image();

				incoming_filetransfers++;
				unlink(cmd__image_filename_full_path); // just in case there are some old leftover bytes there
				incoming_filetransfers_friendnumber = friendnumber;
				incoming_filetransfers_filenumber = filenumber;
				tox_file_control(m, friendnumber, filenumber, TOX_FILE_CONTROL_RESUME, NULL);
			}
			else
			{
				tox_file_control(m, friendnumber, filenumber, TOX_FILE_CONTROL_CANCEL, NULL);
			}
		}
    }
}

void save_resumable_fts(Tox *m, uint32_t friendnum)
{
	size_t i;
	int friendlistnum = find_friend_in_friendlist(friendnum);
	for (i = 0; i < MAX_FILES; ++i)
	{
		// for now save only sending FTs
		struct FileTransfer *ft = &Friends.list[friendlistnum].file_sender[i];
		if (ft->state != FILE_TRANSFER_INACTIVE)
		{
			dbg(9, "save_resumable_fts:saving sender FT i=%d ftnum=%d for friendnum:#%d pos=%d filesize=%d\n", i, (int)ft->filenum, (int)friendnum, (int)ft->position, (int)ft->file_size);
			ll_push(&resumable_filetransfers, sizeof(struct FileTransfer), ft);
			dbg(9, "save_resumable_fts:pushed struct=%p\n", resumable_filetransfers->val);
		}
	}

	Friends.list[friendlistnum].have_resumed_fts = 0;
}



void resume_resumable_fts(Tox *m, uint32_t friendnum)
{
	dbg(9, "resume_resumable_fts:001\n");

    ll_node_t* saved_ft_list = resumable_filetransfers;
    int i = 0;
    while (saved_ft_list != NULL)
	{
		dbg(9, "resume_resumable_fts:element #%d=%p\n", i, saved_ft_list->val);
		if (saved_ft_list->val != NULL)
		{
			struct FileTransfer *ft = (struct FileTransfer*)saved_ft_list->val;
			if (ft->friendnum == friendnum)
			{
				dbg(9, "resume_resumable_fts:**found element #%d=%p\n", i, saved_ft_list->val);
				resume_file_to_friend(m, ft->filenum, ft);
				// now remove element, and start loop again
				ll_remove_by_index(&resumable_filetransfers, i);
				saved_ft_list = resumable_filetransfers;
				i = 0;
				continue;
			}
		}

		i++;
		saved_ft_list = saved_ft_list->next;
	}

	int j = find_friend_in_friendlist(friendnum);
	if (j > -1)
	{
		Friends.list[j].have_resumed_fts = 1;
	}
}



void on_file_chunk_request(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint64_t position,
                           size_t length, void *userdata)
{
    // dbg(9, "on_file_chunk_request:001:friendnum=%d filenum=%d position=%ld len=%d\n", (int)friendnumber, (int)filenumber, (long)position, (int)length);
    struct FileTransfer *ft = get_file_transfer_struct(friendnumber, filenumber);

    if (!ft)
    {
        dbg(0, "on_file_chunk_request:003 ft=NULL\n");
        return;
    }

    if (ft->file_type == TOX_FILE_KIND_AVATAR)
    {
        on_avatar_chunk_request(tox, ft, position, length);
        return;
    }


    if (ft->state != FILE_TRANSFER_STARTED)
    {
        dbg(0, "on_file_chunk_request:005 !FILE_TRANSFER_STARTED\n");
        return;
    }

    if (length == 0)
    {
        dbg(2, "File '%s' successfully sent, ft->state=%d\n", ft->file_name, (int)ft->state);

        char origname[300];
        snprintf(origname, 299, "%s", (const char*)ft->file_name);

        close_file_transfer(tox, ft, -1);
		// also remove the file from disk

		int friendlist_num = find_friend_in_friendlist(friendnumber);
        char longname[300];
        snprintf(longname, 299, "%s/%s", (const char*)Friends.list[friendlist_num].worksubdir, origname);
        dbg(2, "delete file %s\n", longname);
		unlink(longname);

        return;
    }

    if (ft->file == NULL)
    {
        dbg(0, "File transfer for '%s' failed: Null file pointer\n", ft->file_name);
        close_file_transfer(tox, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    if (ft->position != position)
    {
        if (fseek(ft->file, position, SEEK_SET) == -1)
        {
            dbg(0, "File transfer for '%s' failed: Seek fail\n", ft->file_name);
            close_file_transfer(tox, ft, TOX_FILE_CONTROL_CANCEL);
            return;
        }

        ft->position = position;
    }

    uint8_t send_data[length];
    size_t send_length = fread(send_data, 1, sizeof(send_data), ft->file);

    if (send_length != length)
    {
        dbg(0, "File transfer for '%s' failed: Read fail\n", ft->file_name);
        close_file_transfer(tox, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    TOX_ERR_FILE_SEND_CHUNK err;
    tox_file_send_chunk(tox, friendnumber, filenumber, position, send_data, send_length, &err);

    if (err != TOX_ERR_FILE_SEND_CHUNK_OK)
    {
        dbg(0, "tox_file_send_chunk failed in chat callback (error %d)\n", err);
    }

    ft->position += send_length;
    ft->bps += send_length;
    ft->last_keep_alive = get_unix_time();

}


void on_avatar_file_control(Tox *m, struct FileTransfer *ft, TOX_FILE_CONTROL control)
{
    switch (control)
	{
        case TOX_FILE_CONTROL_RESUME:
            if (ft->state == FILE_TRANSFER_PENDING)
			{
                ft->state = FILE_TRANSFER_STARTED;
            }
			else if (ft->state == FILE_TRANSFER_PAUSED)
			{
                ft->state = FILE_TRANSFER_STARTED;
            }

            break;

        case TOX_FILE_CONTROL_PAUSE:
            ft->state = FILE_TRANSFER_PAUSED;
            break;

        case TOX_FILE_CONTROL_CANCEL:
            close_file_transfer(m, ft, -1);
            break;
    }
}


void on_file_control(Tox *m, uint32_t friendnumber, uint32_t filenumber, TOX_FILE_CONTROL control,
                     void *userdata)
{
	if ((incoming_filetransfers_friendnumber == friendnumber) && (incoming_filetransfers_filenumber = filenumber))
	{
		// incoming data file transfer -----------
		switch (control)
		{
			case TOX_FILE_CONTROL_RESUME:
			{
				break;
			}

			case TOX_FILE_CONTROL_PAUSE:
			{
				break;
			}

			case TOX_FILE_CONTROL_CANCEL:
			{
				incoming_filetransfers_friendnumber = -1;
				incoming_filetransfers_filenumber = -1;
				unlink(cmd__image_filename_full_path); // remove leftover data
				incoming_filetransfers = 0;
				break;
			}
		}
		// incoming data file transfer -----------
		return;
	}


    struct FileTransfer *ft = get_file_transfer_struct(friendnumber, filenumber);

    if (!ft)
    {
        return;
    }

    if (ft->file_type == TOX_FILE_KIND_AVATAR)
    {
        on_avatar_file_control(m, ft, control);
        return;
    }

    dbg(9, "on_file_control:002:file in/out\n");



	switch (control)
	{
		case TOX_FILE_CONTROL_RESUME:
		{
			dbg(9, "on_file_control:003:TOX_FILE_CONTROL_RESUME\n");

			ft->last_keep_alive = get_unix_time();

			/* transfer is accepted */
			if (ft->state == FILE_TRANSFER_PENDING)
			{
				ft->state = FILE_TRANSFER_STARTED;
				dbg(9, "on_file_control:004:pending -> started\n");
			}
			else if (ft->state == FILE_TRANSFER_PAUSED)
			{    /* transfer is resumed */
				ft->state = FILE_TRANSFER_STARTED;
				dbg(9, "on_file_control:005:paused -> started\n");
			}

			break;
		}

		case TOX_FILE_CONTROL_PAUSE:
		{
			dbg(9, "on_file_control:006:TOX_FILE_CONTROL_PAUSE\n");
			ft->state = FILE_TRANSFER_PAUSED;
			break;
		}

		case TOX_FILE_CONTROL_CANCEL:
		{
			dbg(1, "File transfer for '%s' was aborted\n", ft->file_name);
			close_file_transfer(m, ft, -1);
			break;
		}
	}

}



void on_avatar_chunk_request(Tox *m, struct FileTransfer *ft, uint64_t position, size_t length)
{
    dbg(9, "on_avatar_chunk_request:001\n");

    if (ft->state != FILE_TRANSFER_STARTED)
    {
        dbg(0, "on_avatar_chunk_request:001a:!FILE_TRANSFER_STARTED\n");
        return;
    }

    if (length == 0)
    {
        close_file_transfer(m, ft, -1);
        return;
    }

    if (ft->file == NULL)
	{
        close_file_transfer(m, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    if (ft->position != position)
	{
        if (fseek(ft->file, position, SEEK_SET) == -1)
		{
            close_file_transfer(m, ft, TOX_FILE_CONTROL_CANCEL);
            return;
        }

        ft->position = position;
    }

    uint8_t send_data[length];
    size_t send_length = fread(send_data, 1, sizeof(send_data), ft->file);

    if (send_length != length)
    {
        close_file_transfer(m, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    TOX_ERR_FILE_SEND_CHUNK err;
    tox_file_send_chunk(m, ft->friendnum, ft->filenum, position, send_data, send_length, &err);

    if (err != TOX_ERR_FILE_SEND_CHUNK_OK)
    {
        dbg(0, "tox_file_send_chunk failed in avatar callback (error %d)\n", err);
    }

    ft->position += send_length;
    ft->last_keep_alive = get_unix_time();
}


void self_connection_status_cb(Tox *tox, TOX_CONNECTION connection_status, void *user_data)
{
    switch (connection_status)
	{
        case TOX_CONNECTION_NONE:
            dbg(2, "Offline\n");
			my_connection_status = TOX_CONNECTION_NONE;
            break;
        case TOX_CONNECTION_TCP:
            dbg(2, "Online, using TCP\n");
			my_connection_status = TOX_CONNECTION_TCP;
            break;
        case TOX_CONNECTION_UDP:
            dbg(2, "Online, using UDP\n");
			my_connection_status = TOX_CONNECTION_UDP;
            break;
    }
}


static struct FileTransfer *new_file_sender(uint32_t friendnum, uint32_t filenum, uint8_t type)
{
    size_t i;

    dbg(9, "new_file_sender:001 friendnum=%d filenum=%d type=%d\n", (int)friendnum, (int) filenum, (int) type);
	int friendlistnum = find_friend_in_friendlist(friendnum);

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft = &Friends.list[friendlistnum].file_sender[i];
        dbg(9, "new_file_sender:002 i=%d\n", (int)i);

        if (ft->state == FILE_TRANSFER_INACTIVE)
        {
			dbg(9, "new_file_sender:003:reusing sender i=%d\n", (int)i);

            memset(ft, 0, sizeof(struct FileTransfer));
			// ft->state = FILE_TRANSFER_INACTIVE; // == 0

            ft->index = i;
            ft->friendnum = friendnum;
            ft->filenum = filenum;
            ft->file_type = type;
            ft->last_keep_alive = get_unix_time();
            ft->state = FILE_TRANSFER_PENDING;
            ft->direction = FILE_TRANSFER_SEND;

            dbg(9, "new_file_sender:003 i=%d\n", (int)i);

            return ft;
        }
    }

    return NULL;
}



static struct FileTransfer *new_file_receiver(uint32_t friendnum, uint32_t filenum, uint8_t type)
{
    size_t i;
	int friendlistnum = find_friend_in_friendlist(friendnum);

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft = &Friends.list[friendlistnum].file_receiver[i];

        if (ft->state == FILE_TRANSFER_INACTIVE) {
            memset(ft, 0, sizeof(struct FileTransfer));
			// ft->state = FILE_TRANSFER_INACTIVE; // == 0

            ft->index = i;
            ft->friendnum = friendnum;
            ft->filenum = filenum;
            ft->file_type = type;
            ft->last_keep_alive = get_unix_time();
            ft->state = FILE_TRANSFER_PENDING;
            ft->direction = FILE_TRANSFER_RECV;
            return ft;
        }
    }

    return NULL;
}


struct FileTransfer *new_file_transfer(uint32_t friendnum, uint32_t filenum,
                                       FILE_TRANSFER_DIRECTION direction, uint8_t type)
{
    if (direction == FILE_TRANSFER_RECV)
    {
        return new_file_receiver(friendnum, filenum, type);
    }

    if (direction == FILE_TRANSFER_SEND)
    {
        return new_file_sender(friendnum, filenum, type);
    }

    return NULL;
}


int avatar_send(Tox *m, uint32_t friendnum)
{
    dbg(2, "avatar_send:001 friendnum=%d\n", (int)friendnum);
    dbg(2, "avatar_send:002 %d %s %d\n", (int)Avatar.size, Avatar.name, (int)Avatar.name_len);

    TOX_ERR_FILE_SEND err;
    uint32_t filenum = tox_file_send(m, friendnum, TOX_FILE_KIND_AVATAR, (size_t) Avatar.size,
                                     NULL, (uint8_t *) Avatar.name, Avatar.name_len, &err);
	dbg(2, "avatar_send:tox_file_send=%s filenum=%d\n", (const char*)Avatar.name, (int)filenum);

    if (Avatar.size == 0)
    {
        return 0;
    }

    if (err != TOX_ERR_FILE_SEND_OK)
    {
        dbg(0, "avatar_send:tox_file_send failed for _friendnumber %d (error %d)\n", friendnum, err);
        return -1;
    }

	dbg(2, "avatar_send(1):tox_file_send=%s filenum=%d\n", (const char*)Avatar.name, (int)filenum);
    struct FileTransfer *ft = new_file_transfer(friendnum, filenum, FILE_TRANSFER_SEND, TOX_FILE_KIND_AVATAR);
	dbg(2, "avatar_send(2):tox_file_send=%s filenum=%d\n", (const char*)Avatar.name, (int)filenum);

    if (!ft)
    {
        dbg(0, "avatar_send:003:ft=NULL\n");
        return -1;
    }

    ft->file = fopen(Avatar.path, "r");

    if (ft->file == NULL)
    {
        dbg(0, "avatar_send:004:ft->file=NULL\n");
        return -1;
    }

    snprintf(ft->file_name, sizeof(ft->file_name), "%s", Avatar.name);
    ft->file_size = Avatar.size;

    return 0;
}


int check_file_signature(const char *signature, size_t size, FILE *fp)
{
    char buf[size];
    if (fread(buf, size, 1, fp) != 1)
    {
        return -1;
    }
    int ret = memcmp(signature, buf, size);
    if (fseek(fp, 0L, SEEK_SET) == -1)
    {
        return -1;
    }
    return ret == 0 ? 0 : 1;
}


void kill_all_file_transfers_friend(Tox *m, uint32_t friendnum)
{
}

void kill_all_file_transfers(Tox *m)
{
}


int avatar_set(Tox *m, const char *path, size_t path_len)
{
    dbg(2, "avatar_set:001\n");

    if (path_len == 0 || path_len >= sizeof(Avatar.path))
    {
        return -1;
    }

    dbg(9, "avatar_set:002\n");
    FILE *fp = fopen(path, "rb");

    if (fp == NULL)
    {
        return -1;
    }

    char PNG_signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    if (check_file_signature(PNG_signature, sizeof(PNG_signature), fp) != 0)
    {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    dbg(9, "avatar_set:003\n");

    off_t size = file_size(path);

    if (size == 0 || size > MAX_AVATAR_FILE_SIZE)
    {
        return -1;
    }

    dbg(9, "avatar_set:004\n");

    get_file_name(Avatar.name, sizeof(Avatar.name), path);
    Avatar.name_len = strlen(Avatar.name);
    snprintf(Avatar.path, sizeof(Avatar.path), "%s", path);
    Avatar.path_len = path_len;
    Avatar.size = size;

    dbg(9, "avatar_set:099\n");

    return 0;
}

static void avatar_clear(void)
{
    memset(&Avatar, 0, sizeof(struct Avatar));
}

void avatar_unset(Tox *m)
{
    avatar_clear();
}

int check_number_of_files_to_resend_to_friend(Tox *m, uint32_t friendnum, int friendlistnum)
{
}

void resend_zip_files_and_send(Tox *m, uint32_t friendnum, int friendlistnum)
{
}

void process_friends_dir(Tox *m, uint32_t friendnum, int friendlistnum)
{
}

void check_friends_dir(Tox *m)
{
}

void check_dir(Tox *m)
{
}


char* get_current_time_date_formatted()
{
	time_t t;
	struct tm *tm = NULL;
	const int max_size_datetime_str = 100;
	char *str_date_time = malloc(max_size_datetime_str);

	memset(str_date_time, 0, 100);
	t = time(NULL);
	tm = localtime(&t);

	strftime(str_date_time, max_size_datetime_str, global_overlay_timestamp_format, tm);

	// dbg(9, "str_date_time=%s\n", str_date_time);

	return str_date_time;
}


int64_t friend_number_for_entry(Tox *tox, uint8_t *tox_id_bin)
{
    size_t i = 0;
    size_t size = tox_self_get_friend_list_size(tox);
    int64_t ret_friendnum = -1;

    if (size == 0)
    {
        return ret_friendnum;
    }

    if (tox_id_bin == NULL)
    {
        return ret_friendnum;
    }

    uint32_t list[size];
    tox_self_get_friend_list(tox, list);
    char friend_key[TOX_PUBLIC_KEY_SIZE];
    CLEAR(friend_key);

    for (i = 0; i < size; ++i)
    {
        if (tox_friend_get_public_key(tox, list[i], (uint8_t *) friend_key, NULL) == 0)
        {
        }
        else
        {
            if (memcmp(tox_id_bin, friend_key, TOX_PUBLIC_KEY_SIZE) == 0)
            {
                ret_friendnum = list[i];
                return ret_friendnum;
            }
        }
    }

    return ret_friendnum;
}


// ------------------- V4L2 stuff ---------------------
// ------------------- V4L2 stuff ---------------------
// ------------------- V4L2 stuff ---------------------


static int xioctl(int fh, unsigned long request, void *arg)
{
    int r;

    do
	{
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}



int init_cam()
{
	int video_dev_open_error = 0;
	int fd;

	if ((fd = open(v4l2_device, O_RDWR)) < 0)
	{
		dbg(0, "error opening video device[1]\n");
		video_dev_open_error = 1;
	}

	if (video_dev_open_error == 1)
	{
		sleep(20); // sleep 20 seconds

		if ((fd = open(v4l2_device, O_RDWR)) < 0)
		{
			dbg(0, "error opening video device[2]\n");
			video_dev_open_error = 1;
		}
		else
		{
			video_dev_open_error = 0;
		}
	}


	struct v4l2_capability cap;
	struct v4l2_cropcap    cropcap;
    // struct v4l2_crop       crop;

	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
	{
		dbg(0, "VIDIOC_QUERYCAP\n");
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
	{
		dbg(0, "The device does not handle single-planar video capture.\n");
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING))
	{
		dbg(0, "The device does not support streaming i/o.\n");
	}


    /* Select video input, video standard and tune here. */
    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
	{
#if 0
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c    = cropcap.defrect; /* reset to full area */
		/* Scale the width and height to 50 % of their original size and center the output. */
		crop.c.width = crop.c.width / 2;
		crop.c.height = crop.c.height / 2;
		crop.c.left = crop.c.left + crop.c.width / 2;
		crop.c.top = crop.c.top + crop.c.height / 2;

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
		{
            switch (errno)
			{
                case EINVAL:
					dbg(0, "Cropping not supported (1)\n");
                    break;
                default:
					dbg(0, "some error on croping setup\n");
                    break;
            }
        }
#endif
    }
	else
	{
		dbg(0, "Cropping not supported (2)\n");
    }


#ifdef V4LCONVERT
    v4lconvert_data = v4lconvert_create(fd);
#endif

    CLEAR(format);
    CLEAR(dest_format);

	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;

	format.fmt.pix.width = 1280;
	format.fmt.pix.height = 720;

	dest_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dest_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;

	dest_format.fmt.pix.width = format.fmt.pix.width;
	dest_format.fmt.pix.height = format.fmt.pix.height;

    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420)
	{
		dbg(2, "Video format(wanted): V4L2_PIX_FMT_YUV420\n");
	}
	else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG)
	{
		dbg(2, "Video format(wanted): V4L2_PIX_FMT_MJPEG\n");
	}
	else
	{
		dbg(2, "Video format(wanted): %u\n", format.fmt.pix.pixelformat);
	}

	// Get <-> Set ??
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &format))
	{
		dbg(0, "VIDIOC_G_FMT\n");
	}

    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420)
	{
		dbg(2, "Video format(got): V4L2_PIX_FMT_YUV420\n");
	}
	else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG)
	{
		dbg(2, "Video format(got): V4L2_PIX_FMT_MJPEG\n");
	}
	else
	{
		dbg(2, "Video format(got): %u\n", format.fmt.pix.pixelformat);
	}

	if (video_high == 1)
	{
		format.fmt.pix.width = 1280;
		format.fmt.pix.height = 720;
	}
	else
	{
		format.fmt.pix.width = 640;
		format.fmt.pix.height = 480;
	}

    video_width             = format.fmt.pix.width;
    video_height            = format.fmt.pix.height;
	dbg(2, "Video size(wanted): %u %u\n", video_width, video_height);

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &format))
	{
		dbg(0, "VIDIOC_S_FMT\n");
	}

	if (-1 == xioctl(fd, VIDIOC_G_FMT, &format))
	{
		dbg(0, "VIDIOC_G_FMT\n");
	}

    video_width             = format.fmt.pix.width;
    video_height            = format.fmt.pix.height;
	dbg(2, "Video size(got): %u %u\n", video_width, video_height);

	dest_format.fmt.pix.width = format.fmt.pix.width;
	dest_format.fmt.pix.height = format.fmt.pix.height;


    /* Buggy driver paranoia. */
/*
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min                          = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;
*/


	struct v4l2_requestbuffers bufrequest;

	CLEAR(bufrequest);

	bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufrequest.memory = V4L2_MEMORY_MMAP;
	bufrequest.count = VIDEO_BUFFER_COUNT;

	dbg(0, "VIDIOC_REQBUFS want type=%d\n", (int)bufrequest.type);

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &bufrequest))
	{
        if (EINVAL == errno)
		{
            dbg(0, "%s does not support x i/o\n", v4l2_device);
        }
		else
		{
            // dbg(0, "VIDIOC_REQBUFS error %d, %s\n", errno, strerror(errno));
			// try again ...
			if (-1 == xioctl(fd, VIDIOC_REQBUFS, &bufrequest))
			{
				if (EINVAL == errno)
				{
					dbg(0, "[2nd] %s does not support x i/o\n", v4l2_device);
				}
				else
				{
					dbg(0, "[2nd] VIDIOC_REQBUFS error %d, %s\n", errno, strerror(errno));
				}
			}
        }
    }

	dbg(0, "VIDIOC_REQBUFS got type=%d\n", (int)bufrequest.type);

    if (bufrequest.count < 2)
	{
        dbg(0, "Insufficient buffer memory on %s\n", v4l2_device);
    }


	buffers = calloc(bufrequest.count, sizeof(*buffers));

	// dbg(0, "VIDIOC_REQBUFS number of buffers[1]=%d\n", (int)bufrequest.count);
	// dbg(0, "VIDIOC_REQBUFS number of buffers[2]=%d\n", (int)n_buffers);

	for (n_buffers = 0; n_buffers < bufrequest.count; ++n_buffers)
	{
		struct v4l2_buffer bufferinfo;

		CLEAR(bufferinfo);

		bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		bufferinfo.memory = V4L2_MEMORY_MMAP;
		bufferinfo.index = n_buffers;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &bufferinfo))
		{
            dbg(9, "VIDIOC_QUERYBUF (2) error %d, %s\n", errno, strerror(errno));
        }
		// else
		//{
        //    dbg(9, "VIDIOC_QUERYBUF (2) *OK*  %d, %s\n", errno, strerror(errno));
		//}

/*
		if (ioctl(fd, VIDIOC_QUERYBUF, &bufferinfo) < 0)
		{
			dbg(0, "VIDIOC_QUERYBUF %d %s\n", errno, strerror(errno));
		}
*/

        buffers[n_buffers].length = bufferinfo.length;
        buffers[n_buffers].start  = mmap(NULL /* start anywhere */, bufferinfo.length, PROT_READ | PROT_WRITE /* required */,
                                        MAP_SHARED /* recommended */, fd, bufferinfo.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
		{
            dbg(0, "mmap error %d, %s\n", errno, strerror(errno));
        }

	}

	// dbg(0, "VIDIOC_REQBUFS number of buffers[2b]=%d\n", (int)n_buffers);

	return fd;
}


int v4l_startread()
{
    dbg(9, "start cam\n");
    size_t i;
    enum v4l2_buf_type type;

    // dbg(0, "VIDIOC_REQBUFS number of buffers[2x]=%d\n", (int)n_buffers);

    for (i = 0; i < n_buffers; ++i)
	{
		struct v4l2_buffer buf;

		dbg(9, "buffer (1) %d of %d\n", i, n_buffers);

        CLEAR(buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (-1 == xioctl(global_cam_device_fd, VIDIOC_QBUF, &buf))
	{
            dbg(9, "VIDIOC_QBUF (3) error %d, %s\n", errno, strerror(errno));
            return 0;
        }
	// else
	//{
    //        dbg(9, "VIDIOC_QBUF (3) *OK*  %d, %s\n", errno, strerror(errno));
	//}
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(global_cam_device_fd, VIDIOC_STREAMON, &type))
	{
        dbg(9, "VIDIOC_STREAMON error %d, %s\n", errno, strerror(errno));
        return 0;
    }

    return 1;
}


int v4l_endread()
{
    dbg(9, "stop webcam\n");
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(global_cam_device_fd, VIDIOC_STREAMOFF, &type))
	{
        dbg(9, "VIDIOC_STREAMOFF error %d, %s\n", errno, strerror(errno));
        return 0;
    }

    return 1;
}


void yuv422to420(uint8_t *plane_y, uint8_t *plane_u, uint8_t *plane_v, uint8_t *input, uint16_t width, uint16_t height)
{
    uint8_t *end = input + width * height * 2;
    while (input != end)
	{
        uint8_t *line_end = input + width * 2;
        while (input != line_end)
		{
            *plane_y++ = *input++;
            *plane_v++ = *input++;
            *plane_y++ = *input++;
            *plane_u++ = *input++;
        }

        line_end = input + width * 2;
        while (input != line_end)
		{
            *plane_y++ = *input++;
            input++; // u
            *plane_y++ = *input++;
            input++; // v
        }
    }
}


int v4l_getframe(uint8_t *y, uint8_t *u, uint8_t *v, uint16_t width, uint16_t height)
{
    if (width != video_width || height != video_height)
	{
        dbg(9, "V4L:\twidth/height mismatch %u %u != %u %u\n", width, height, video_width, video_height);
        return 0;
    }

    struct v4l2_buffer buf;
    // ** // CLEAR(buf);

    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP; // V4L2_MEMORY_USERPTR;

    if (-1 == ioctl(global_cam_device_fd, VIDIOC_DQBUF, &buf))
	{
        switch (errno)
		{
            case EINTR:
            case EAGAIN: return 0;

            case EIO:
            /* Could ignore EIO, see spec. */

            /* fall through */

            default:
	     // dbg(9, "VIDIOC_DQBUF error %d, %s\n", errno, strerror(errno));
		return -1;

        }
    }

    /*for (i = 0; i < n_buffers; ++i)
        if (buf.m.userptr == (unsigned long)buffers[i].start
                && buf.length == buffers[i].length)
            break;

    if(i >= n_buffers) {
        dbg(9, "fatal error\n");
        return 0;
    }*/

	// dbg(9, "buf.index=%d\n", (int)buf.index);

    void *data = (void *)buffers[buf.index].start; // length = buf.bytesused //(void*)buf.m.userptr

/* assumes planes are continuous memory */
#ifdef V4LCONVERT
    // dbg(9, "V4LCONVERT\n");
    int result = v4lconvert_convert(v4lconvert_data, &format, &dest_format, data, buf.bytesused, y,
                                    (video_width * video_height * 3) / 2);

    if (result == -1)
	{
        dbg(0, "v4lconvert_convert error %s\n", v4lconvert_get_error_message(v4lconvert_data));
    }
#else
    dbg(9, "convert2\n");
    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
	{
        dbg(9, "yuv422to420\n");
        yuv422to420(y, u, v, data, video_width, video_height);
    }
	else
	{
    }
#endif

    if (-1 == xioctl(global_cam_device_fd, VIDIOC_QBUF, &buf))
	{
        dbg(9, "VIDIOC_QBUF (1) error %d, %s\n", errno, strerror(errno));
    }

#ifdef V4LCONVERT
    return (result == -1 ? 0 : 1);
#else
    return 1;
#endif
}


void close_cam()
{

#ifdef HAVE_FRAMEBUFFER
	// close framebuffer device
	dbg(2, "munmaping Framebuffer\n");

	if (framebuffer_mappedmem != NULL)
	{
		int res = munmap(framebuffer_mappedmem, (size_t)framebuffer_screensize);
		framebuffer_mappedmem = NULL;
		dbg(9, "munmap Framebuffer error\n");
	}

	dbg(2, "closing Framebuffer\n");
	if (global_framebuffer_device_fd > 0)
	{
		close(global_framebuffer_device_fd);
		global_framebuffer_device_fd = 0;
	}

//	if (bf_out_data != NULL)
//	{
//		free(bf_out_data);
//		bf_out_data = NULL;
//	}
#endif

#ifdef HAVE_LIBAO
	if (_ao_device != NULL)
	{
		ao_close(_ao_device);
	}
	ao_shutdown();
#endif

#ifdef HAVE_PORTAUDIO

	ringbuf_free(&portaudio_out_rb);

	PaError err;

	err = Pa_StopStream(portaudio_stream);
	if (err != paNoError)
	{
		dbg(0, "Pa_StopStream error\n");
	}

	err = Pa_CloseStream(portaudio_stream);
    if (err != paNoError)
	{
		dbg(0, "Pa_CloseStream error\n");
	}

	Pa_Terminate();
#endif

#ifdef V4LCONVERT
	v4lconvert_destroy(v4lconvert_data);
#endif

    size_t i;
    for (i = 0; i < n_buffers; ++i)
	{
        if (-1 == munmap(buffers[i].start, buffers[i].length))
		{
            dbg(9, "munmap error\n");
        }
    }

    close(global_cam_device_fd);
}

// ------------------- V4L2 stuff ---------------------
// ------------------- V4L2 stuff ---------------------
// ------------------- V4L2 stuff ---------------------



// ------------------ Tox AV stuff --------------------
// ------------------ Tox AV stuff --------------------
// ------------------ Tox AV stuff --------------------

static void t_toxav_call_cb(ToxAV *av, uint32_t friend_number, bool audio_enabled, bool video_enabled, void *user_data)
{
	if (accepting_calls != 1)
	{
		dbg(2, "Not accepting calls yet\n");
		return;
	}


	if (global_video_active == 1)
	{
		dbg(9, "Call already active\n");
	}
	else
	{
		dbg(9, "Handling CALL callback friendnum=%d audio_enabled=%d video_enabled=%d\n", (int)friend_number, (int)audio_enabled, (int)video_enabled);
		((CallControl *)user_data)->incoming = true;

		TOXAV_ERR_ANSWER err;
		global_video_bit_rate = DEFAULT_GLOBAL_VID_BITRATE;

		update_status_line_1_text();

		int audio_bitrate = DEFAULT_GLOBAL_AUD_BITRATE;
		int video_bitrate = global_video_bit_rate;
		friend_to_send_video_to = friend_number;
		global_video_active = 1;
		global_send_first_frame = 2;

		dbg(9, "Handling CALL callback friendnum=%d audio_bitrate=%d video_bitrate=%d global_video_active=%d\n", (int)friend_number, (int)audio_bitrate, (int)video_bitrate, global_video_active);

		show_text_as_image_stop();
		
		toxav_answer(av, friend_number, audio_bitrate, video_bitrate, &err);

		// clear screen on CALL ANSWER
		stop_endless_image();
		fb_fill_black();
		// show funny face
		show_video_calling();

	}
}

static void t_toxav_call_state_cb(ToxAV *av, uint32_t friend_number, uint32_t state, void *user_data)
{
	if (accepting_calls != 1)
	{
		dbg(2, "Not accepting call state changes yet\n");
		return;
	}

	if ((friend_to_send_video_to != friend_number) && (global_video_active == 1))
	{
		// we are in a call with someone else already
		dbg(9, "We are in a call with someone else already. trying fn=%d\n", (int)friend_number);
		return;
	}

    dbg(9, "Handling CALL STATE callback: %d friend_number=%d\n", state, (int)friend_number);

    ((CallControl *)user_data)->state = state;

	if (state & TOXAV_FRIEND_CALL_STATE_FINISHED)
	{
		global_video_active = 0;
		dbg(9, "Call with friend %d finished, global_video_active=%d\n", friend_number, global_video_active);
		friend_to_send_video_to = -1;

		show_tox_id_qrcode();
		return;
	}
	else if (state & TOXAV_FRIEND_CALL_STATE_ERROR)
	{
		global_video_active = 0;
		dbg(9, "Call with friend %d errored, global_video_active=%d\n", friend_number, global_video_active);
		friend_to_send_video_to = -1;

		show_tox_id_qrcode();
		return;
	}
	else if (state & TOXAV_FRIEND_CALL_STATE_SENDING_A)
	{
		dbg(9, "Call with friend state:TOXAV_FRIEND_CALL_STATE_SENDING_A\n");
	}
	else if (state & TOXAV_FRIEND_CALL_STATE_SENDING_V)
	{
		dbg(9, "Call with friend state:TOXAV_FRIEND_CALL_STATE_SENDING_V\n");
	}
	else if (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_A)
	{
		dbg(9, "Call with friend state:TOXAV_FRIEND_CALL_STATE_ACCEPTING_A\n");
	}
	else if (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_V)
	{
		dbg(9, "Call with friend state:TOXAV_FRIEND_CALL_STATE_ACCEPTING_V\n");
	}

	dbg(9, "t_toxav_call_state_cb:002\n");
	int send_audio = (state & TOXAV_FRIEND_CALL_STATE_SENDING_A) && (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_A);
	int send_video = state & TOXAV_FRIEND_CALL_STATE_SENDING_V && (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_V);
	dbg(9, "t_toxav_call_state_cb:002a send_audio=%d send_video=%d global_video_bit_rate=%d\n", send_audio, send_video, (int)global_video_bit_rate);
	TOXAV_ERR_BIT_RATE_SET bitrate_err = 0;
	// ** // toxav_bit_rate_set(av, friend_number, 0, send_video ? global_video_bit_rate : 0, &bitrate_err);
	dbg(9, "t_toxav_call_state_cb:003\n");

	if (bitrate_err)
	{
		dbg(9, "ToxAV:Error setting/changing video bitrate\n");
	}

	// HINT: no matter what show the "call" screen
	send_video = 1;
	// HINT: no matter what show the "call" screen

	if (send_video == 1)
	{
		global_is_qrcode_showing_on_screen = 0;

		if (global_video_active == 0)
		{
			// clear screen on CALL START
			stop_endless_image();
			fb_fill_black();
			// show funny face
			show_video_calling();

			dbg(9, "t_toxav_call_state_cb:004xx\n");
			global_video_active = 1;
			global_send_first_frame = 2;

			dbg(9, "global_video_active=%d friend_to_send_video_to=%d\n", global_video_active, friend_to_send_video_to);
		}
	}
	// -------- never reached --------
	// -------- never reached --------
	//else
	//{
	//	dbg(9, "t_toxav_call_state_cb:005\n");
	//	global_video_active = 0;
	//	global_send_first_frame = 0;
	//	friend_to_send_video_to = -1;
	//
	//	show_tox_id_qrcode();
	//}
	// -------- never reached --------
	// -------- never reached --------

	dbg(9, "Call state for friend %d changed to %d, audio=%d, video=%d global_video_active=%d global_send_first_frame=%d friend_to_send_video_to=%d\n", friend_number, state, send_audio, send_video, global_video_active, global_send_first_frame, friend_to_send_video_to);
}

static void t_toxav_bit_rate_status_cb(ToxAV *av, uint32_t friend_number,
                                       uint32_t audio_bit_rate, uint32_t video_bit_rate,
                                       void *user_data)
{
	//if ((friend_to_send_video_to != friend_number) && (global_video_active == 1))
	//{
	//	// we are in a call with someone else already
	//	dbg(9, "We are in a call with someone else already. trying fn=%d\n", (int)friend_number);
	//	return;
	//}


	dbg(0, "t_toxav_bit_rate_status_cb:001 video_bit_rate=%d\n", (int)video_bit_rate);
	dbg(0, "t_toxav_bit_rate_status_cb:001 audio_bit_rate=%d\n", (int)audio_bit_rate);

	TOXAV_ERR_BIT_RATE_SET error = 0;


	uint32_t video_bit_rate_ = video_bit_rate;

	if (video_bit_rate < DEFAULT_GLOBAL_MIN_VID_BITRATE)
	{
		video_bit_rate_ = DEFAULT_GLOBAL_MIN_VID_BITRATE;
	}

	toxav_bit_rate_set(av, friend_number, audio_bit_rate, video_bit_rate_, &error);

	if (error != 0)
	{
		dbg(0, "ToxAV:Setting new Video bitrate has failed with error #%u\n", error);
	}
	else
	{
		// HINT: don't touch global video bitrate --------
		// global_video_bit_rate = video_bit_rate_;
		// HINT: don't touch global video bitrate --------

		// TODO: this will get overwritten at every fps update!!
		update_status_line_1_text_arg(video_bit_rate_);

	}

    dbg(2, "suggested bit rates: audio: %d video: %d\n", audio_bit_rate, video_bit_rate);
    dbg(2, "actual    bit rates: audio: %d video: %d\n", global_audio_bit_rate, global_video_bit_rate);
}

#ifdef HAVE_ALSA_PLAY
void inc_audio_t_counter()
{
	sem_wait(&count_audio_play_threads);
	count_audio_play_threads_int++;
	sem_post(&count_audio_play_threads);
}

void dec_audio_t_counter()
{
	sem_wait(&count_audio_play_threads);
	count_audio_play_threads_int--;
	if (count_audio_play_threads_int < 0)
	{
		count_audio_play_threads_int = 0;
	}
	sem_post(&count_audio_play_threads);
}

int get_audio_t_counter()
{
	sem_wait(&count_audio_play_threads);
	int ret = count_audio_play_threads_int;
	sem_post(&count_audio_play_threads);
	return ret;
}
#endif


#ifdef HAVE_LIBAO
void inc_audio_t_counter()
{
	sem_wait(&count_audio_play_threads);
	count_audio_play_threads_int++;
	sem_post(&count_audio_play_threads);
}

void dec_audio_t_counter()
{
	sem_wait(&count_audio_play_threads);
	count_audio_play_threads_int--;
	if (count_audio_play_threads_int < 0)
	{
		count_audio_play_threads_int = 0;
	}
	sem_post(&count_audio_play_threads);
}

int get_audio_t_counter()
{
	sem_wait(&count_audio_play_threads);
	int ret = count_audio_play_threads_int;
	sem_post(&count_audio_play_threads);
	return ret;
}
#endif



void inc_video_t_counter()
{
	sem_wait(&count_video_play_threads);
	count_video_play_threads_int++;
	sem_post(&count_video_play_threads);
}

void dec_video_t_counter()
{
	sem_wait(&count_video_play_threads);
	count_video_play_threads_int--;
	if (count_video_play_threads_int < 0)
	{
		count_video_play_threads_int = 0;
	}
	sem_post(&count_video_play_threads);
}

int get_video_t_counter()
{
	sem_wait(&count_video_play_threads);
	int ret = count_video_play_threads_int;
	sem_post(&count_video_play_threads);
	return ret;
}


void inc_video_trec_counter()
{
	sem_wait(&count_video_record_threads);
	count_video_record_threads_int++;
	sem_post(&count_video_record_threads);
}

void dec_video_trec_counter()
{
	sem_wait(&count_video_record_threads);
	count_video_record_threads_int--;
	if (count_video_record_threads_int < 0)
	{
		count_video_record_threads_int = 0;
	}
	sem_post(&count_video_record_threads);
}

int get_video_trec_counter()
{
	sem_wait(&count_video_record_threads);
	int ret = count_video_record_threads_int;
	sem_post(&count_video_record_threads);
	return ret;
}




#ifdef HAVE_ALSA_PLAY

void *alsa_audio_play(void *data)
{

	struct alsa_audio_play_data_block *adb = (struct alsa_audio_play_data_block *)data;

	dbg(0, "ALSA:001\n");

#if 0
	// ------ thread priority ------
	struct sched_param param;
	int policy;
	int s;
	display_thread_sched_attr("Scheduler attributes of [1]: alsa_audio_play thread");
#endif

	// make a local copy
	char *play_pcm_ = (char *)calloc(1, adb->block_size_in_bytes);

	if (play_pcm_)
	{
		memcpy(play_pcm_, adb->pcm, adb->block_size_in_bytes);

		sem_wait(&audio_play_lock);
		int err;
		if ((err = snd_pcm_writei(audio_play_handle, (char *)play_pcm_, adb->sample_count)) != adb->sample_count)
		{
			dbg(0, "play_device:write to audio interface failed (err=%d) (%s)\n", (int)err, snd_strerror(err));
			if ((int)err == -11) // -> Resource temporarily unavailable
			{
				dbg(0, "play_device:yield a bit (2)\n");
				// zzzzzz
				yieldcpu(1);
			}
			sound_play_xrun_recovery(audio_play_handle, err, (int)adb->channels, (int)adb->sampling_rate);
		}
		sem_post(&audio_play_lock);

		free(play_pcm_);
	}

	free(adb);

	dec_audio_t_counter();

	pthread_exit(0);
}

#endif



#ifdef HAVE_LIBAO

void *audio_play(void *data)
{
	struct audio_play_data_block *adb = (struct audio_play_data_block *)data;

#if 0
	// ------ thread priority ------
	struct sched_param param;
	int policy;
	int s;
	display_thread_sched_attr("Scheduler attributes of [1]: audio_play thread");
#endif

	// make a local copy
	char *ao_play_pcm_ = (char *)malloc(adb->block_size_in_bytes);
	memcpy(ao_play_pcm_, adb->pcm, adb->block_size_in_bytes);

	// this is a blocking call
	if (libao_cancel_pending == 0)
	{
		sem_wait(&audio_play_lock);
		ao_play( _ao_device, (char *)ao_play_pcm_, (uint_32)adb->block_size_in_bytes);
		sem_post(&audio_play_lock);
	}

	free(ao_play_pcm_);
	free(adb);

	dec_audio_t_counter();

	pthread_exit(0);
}

#endif


static void t_toxav_receive_audio_frame_cb(ToxAV *av, uint32_t friend_number,
        int16_t const *pcm,
        size_t sample_count,
        uint8_t channels,
        uint32_t sampling_rate,
        void *user_data)
{
	if (global_video_active == 1)
	{
		if (friend_to_send_video_to == friend_number)
		{

#ifdef HAVE_ALSA_PLAY
			if ((libao_channels != channels) || (libao_sampling_rate != sampling_rate))
			{

#if 0
				// ------ thread priority ------
				struct sched_param param;
				int policy;
				int s;
				display_thread_sched_attr("Scheduler attributes of [1]: alsa audio play thread");
				get_policy('f', &policy);
				param.sched_priority = strtol("1", NULL, 0);
				s = pthread_setschedparam(pthread_self(), policy, &param);
				if (s != 0)
				{
					dbg(0, "Scheduler attributes of [2]: error setting scheduling attributes of alsa audio play thread\n");
				}
				else
				{
				}
				display_thread_sched_attr("Scheduler attributes of [3]: alsa audio play thread");
				// ------ thread priority ------
#endif


				sem_wait(&audio_play_lock);

				libao_channels = (int)channels;
				libao_sampling_rate = (int)sampling_rate;

				// initialize sound output ------------------
				close_sound_play_device();
				// zzzzzz
				yieldcpu(20);
				init_sound_play_device((int)libao_channels, (int)libao_sampling_rate);
				// initialize sound output ------------------

				sem_post(&audio_play_lock);

			}

#endif

#ifdef HAVE_LIBAO

			if ((libao_channels != channels)||(libao_sampling_rate != sampling_rate))
			{
#if 1
				// ------ thread priority ------
				struct sched_param param;
				int policy;
				int s;
				display_thread_sched_attr("Scheduler attributes of [1]: audio play thread");
				get_policy('r', &policy);
				param.sched_priority = strtol("1", NULL, 0);
				s = pthread_setschedparam(pthread_self(), policy, &param);
				if (s != 0)
				{
					dbg(0, "Scheduler attributes of [2]: error setting scheduling attributes of audio play thread\n");
				}
				else
				{
				}
				display_thread_sched_attr("Scheduler attributes of [3]: audio play thread");
				// ------ thread priority ------
#endif

				sem_wait(&audio_play_lock);

				libao_cancel_pending = 1;

				libao_channels = (int)channels;
				libao_sampling_rate = (int)sampling_rate;

				if (_ao_device != NULL)
				{
					dbg(0, "closing sound output device\n");
					ao_close(_ao_device);
				}

				// initialize sound output via libao ------------------
				ao_initialize();
				_ao_default_driver = ao_default_driver_id();
				memset(&_ao_format, 0, sizeof(_ao_format));
				_ao_format.bits = 16;
				_ao_format.channels = libao_channels;
				_ao_format.rate = libao_sampling_rate;
				_ao_format.byte_format = AO_FMT_LITTLE;

				dbg(0, "reconfiguring sound output device: channels=%d, rate=%d\n", (int)libao_channels, (int)libao_sampling_rate);
				_ao_device = ao_open_live(_ao_default_driver, &_ao_format, NULL /* no options */);

				if (_ao_device == NULL)
				{
					dbg(0, "Error opening sound output device\n");
				}
				else
				{
				}
				// initialize sound output via libao ------------------

				sem_post(&audio_play_lock);


				yieldcpu(500);
				libao_cancel_pending = 0;
			}
#endif

#ifdef HAVE_ALSA_PLAY

#if 1

			sem_wait(&audio_play_lock);

			int err;
			int has_error = 0;
			int avail_bytes_in_play_buffer = (int)snd_pcm_avail_update(audio_play_handle);
			// dbg(9, "snd_pcm_avail [1]:%d sample_count=%d\n", avail_bytes_in_play_buffer, sample_count);

			// dbg(0, "ALSA:013 sample_count=%d pcmbuf=%p\n", sample_count, (void *)pcm);


			if ((int)avail_bytes_in_play_buffer > (int)sample_count)
			{
				err = snd_pcm_writei(audio_play_handle, (char *)pcm, sample_count);
				has_error = 0;
			}
			else
			{
				err = avail_bytes_in_play_buffer;
				has_error = 1;
			}

			if ((has_error == 1) || (err != sample_count))
			{
				// dbg(0, "play_device:write to audio interface failed (err=%d) (%s)\n", (int)err, snd_strerror(err));

				// dbg(0, "play_device:write to audio interface failed (err=%d) (%s)\n", (int)err, snd_strerror(err));
				//if (err == -11) // -> Resource temporarily unavailable
				//{
				//	dbg(0, "play_device:Resource temporarily unavailable (1)\n");
				//	// zzzzzz
				//	yieldcpu(1);
				//}
				if (err == -EAGAIN)
				{
					dbg(0, "play_device:EAGAIN\n");
					// zzzzzz
					yieldcpu(1);
				}
				else
				{
					sound_play_xrun_recovery(audio_play_handle, err, (int)libao_channels, (int)libao_sampling_rate);
				}
			}

			sem_post(&audio_play_lock);


			// avail_bytes_in_play_buffer = (int)snd_pcm_avail_update(audio_play_handle);
			// dbg(9, "snd_pcm_avail [2]:%d\n", avail_bytes_in_play_buffer);

			// dbg(0, "ALSA:014\n");


#else

			struct alsa_audio_play_data_block *adb = calloc(1, sizeof (struct alsa_audio_play_data_block));
			adb->block_size_in_bytes = (size_t)(sample_count * libao_channels * 2);
			adb->pcm = (char *)pcm;
			adb->channels = libao_channels;
			adb->sampling_rate = libao_sampling_rate;
			adb->sample_count = sample_count;

			pthread_t audio_play_thread;
			if (get_audio_t_counter() <= MAX_ALSA_AUDIO_PLAY_THREADS)
			{
				inc_audio_t_counter();
				if (pthread_create(&audio_play_thread, NULL, alsa_audio_play, (void *)adb))
				{
					dbg(0, "error creating audio play thread\n");
					free(adb);
					dec_audio_t_counter();
				}
				else
				{
					if (pthread_detach(audio_play_thread))
					{
						dbg(0, "error detaching audio play thread\n");
					}

					// zzzzzz
					yieldcpu(1);
				}
			}
			else
			{
				dbg(1, "more than %d alsa play threads already\n", (int)MAX_ALSA_AUDIO_PLAY_THREADS);
				free(adb);
			}
#endif

#endif

#ifdef HAVE_LIBAO
			// play audio to default audio device --------------
			if (_ao_device != NULL)
			{
#if 0
				ao_play( _ao_device, (char *)pcm, (size_t)(sample_count * libao_channels * 2) );
#else

				ao_play_pcm = (char *)pcm;
				ao_play_bytes = (size_t)(sample_count * libao_channels * 2);

				struct audio_play_data_block *adb = calloc(1, sizeof (struct audio_play_data_block));
				adb->block_size_in_bytes = ao_play_bytes;
				adb->pcm = (char *)pcm;
				// adb->sample_count = sample_count; // not used now

				// adb->pcm = calloc(1, adb->block_size_in_bytes);
				// memcpy(adb->pcm, pcm, adb->block_size_in_bytes);

				// dbg(0, "ao_play_bytes=%d sample_count=%d channels=%d samplerate=%d\n", (int)ao_play_bytes, (int)sample_count, (int)libao_channels, (int)libao_sampling_rate);

				pthread_t audio_play_thread;

				if (get_audio_t_counter() <= MAX_AO_PLAY_THREADS)
				{
					inc_audio_t_counter();
					if (pthread_create(&audio_play_thread, NULL, audio_play, (void *)adb))
					{
						dbg(0, "error creating audio play thread\n");

						// free(adb->pcm);
						free(adb);

						dec_audio_t_counter();
					}
					else
					{
						// dbg(0, "creating audio play thread #%d\n", get_audio_t_counter());
						if (pthread_detach(audio_play_thread))
						{
							dbg(0, "error detaching audio play thread\n");
						}

						// zzzzzz
						yieldcpu(1);
					}
				}
				else
				{
					dbg(1, "more than %d ao play threads already\n", (int)MAX_AO_PLAY_THREADS);

					// free(adb->pcm);
					free(adb);
				}
#endif
			}
			// play audio to default audio device --------------
#endif


#ifdef HAVE_PORTAUDIO

			PaError is_stream_active = 0;
			if (portaudio_stream)
			{
				is_stream_active = Pa_IsStreamActive(portaudio_stream);
			}

			int need_reconfig = 0;
			if ((libao_channels != channels) || (libao_sampling_rate != sampling_rate))
			{
				libao_channels = (int)channels;
				libao_sampling_rate = (int)sampling_rate;
				need_reconfig = 1;
				dbg(2, "need to reconfigure audio stream\n");
			}

			if ((need_reconfig == 1) && (is_stream_active == 1))
			{
				PaError err_abort = Pa_AbortStream(portaudio_stream);
				if (err_abort != paNoError)
				{
					dbg(0, "Error: calling Pa_AbortStream\n");
				}
			}

			if ((need_reconfig == 1) || (is_stream_active != 1))
			{
				PaError err;

				if (need_reconfig != 1)
				{
					dbg(0, "starting sound output device: channels=%d, rate=%d\n", (int)libao_channels, (int)libao_sampling_rate);
				}
				else
				{
					dbg(0, "reconfiguring sound output device: channels=%d, rate=%d\n", (int)libao_channels, (int)libao_sampling_rate);
				}

				PaStreamParameters outputParameters;
				outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
				if (outputParameters.device == paNoDevice)
				{
					dbg(0, "Error: No default output device\n");
				}

				outputParameters.channelCount = (int)libao_channels;       /* stereo output */
				outputParameters.sampleFormat = paInt16; /* 16 bit output */
				outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
				outputParameters.hostApiSpecificStreamInfo = NULL;

				//
				// number of bytes   --> (size_t)(sample_count * channels * 2)
				// number of samples --> sample_count
				//
				err = Pa_OpenStream(
				   &portaudio_stream,
				   NULL, /* no input */
				   &outputParameters,
				   (int)libao_sampling_rate,
				   paFramesPerBufferUnspecified,
				   paDitherOff, /* Clip but don't dither */
				   portaudio_data_callback,
				   NULL );

				if (err != paNoError)
				{
					dbg(0, "Error: calling Pa_OpenStream\n");
				}

				err = Pa_StartStream( portaudio_stream );
				if (err != paNoError)
				{
					dbg(0, "Error: calling Pa_StartStream\n");
				}
			}


			// now actually put the audio data into the ring buffer
			ringbuf_memcpy_into(portaudio_out_rb, (const char *)pcm, (size_t)(sample_count * channels * 2));

#endif

		}
		else
		{
			// wrong friend
		}
	}
	else
	{
	}

    // CallControl *cc = (CallControl *)user_data;
    // frame *f = (frame *)malloc(sizeof(uint16_t) + sample_count * sizeof(int16_t) * channels);
    // memcpy(f->data, pcm, sample_count * sizeof(int16_t) * channels);
    // f->size = sample_count;

    // pthread_mutex_lock(cc->arb_mutex);
    // free(rb_write(cc->arb, f));
    // pthread_mutex_unlock(cc->arb_mutex);
}


void update_status_line_on_fb()
{
	unsigned char *bf_out_real_fb = framebuffer_mappedmem;

	text_on_bgra_frame_xy(var_framebuffer_info.xres, var_framebuffer_info.yres,
		var_framebuffer_fix_info.line_length, bf_out_real_fb,
		10, var_framebuffer_info.yres - 50, status_line_1_str);

	text_on_bgra_frame_xy(var_framebuffer_info.xres, var_framebuffer_info.yres,
		var_framebuffer_fix_info.line_length, bf_out_real_fb,
		10, var_framebuffer_info.yres - 30, status_line_2_str);

}


void *video_play(void *dummy)
{
	sem_wait(&video_play_lock);

#if 0
	int num = get_video_t_counter();

    char thread_name_str[15];
	CLEAR(thread_name_str);
    snprintf(thread_name_str, sizeof(thread_name_str), "v_p_thrd #%d", (int)num);
	pthread_setname_np(pthread_self(), thread_name_str);
#else
	// pthread_setname_np(pthread_self(), "v_p_thrd+");
#endif

	// make a local copy
	uint16_t width = video__width;
	uint16_t height = video__height;
	int32_t ystride = video__ystride;
	int32_t ustride = video__ustride;
	int32_t vstride = video__vstride;


				// dbg(0, "receive_video_frame:fnum=%d\n", (int)friend_number);

				int frame_width_px1 = (int)width;
				int frame_height_px1 = (int)height;

				int ystride_ = (int)ystride;
				int ustride_ = (int)ustride;
				int vstride_ = (int)vstride;

				/*
				* YUV420 frame with width * height
				*
				* @param y Luminosity plane. Size = MAX(width, abs(ystride)) * height.
				* @param u U chroma plane. Size = MAX(width/2, abs(ustride)) * (height/2).
				* @param v V chroma plane. Size = MAX(width/2, abs(vstride)) * (height/2).
				*/
				int y_layer_size = (int) max(frame_width_px1, abs(ystride_)) * frame_height_px1;
				int u_layer_size = (int) max((frame_width_px1 / 2), abs(ustride_)) * (frame_height_px1 / 2);
				int v_layer_size = (int) max((frame_width_px1 / 2), abs(vstride_)) * (frame_height_px1 / 2);


	uint8_t *y = (uint8_t *)calloc(1, y_layer_size);
	uint8_t *u = (uint8_t *)calloc(1, u_layer_size);
	uint8_t *v = (uint8_t *)calloc(1, v_layer_size);

	memcpy(y, video__y, y_layer_size);
	memcpy(u, video__u, u_layer_size);
	memcpy(v, video__v, v_layer_size);

	sem_post(&video_play_lock);



				int frame_width_px = (int) max(frame_width_px1, abs(ystride_));
				int frame_height_px = (int) frame_height_px1;

				full_width = var_framebuffer_info.xres;
				full_height = var_framebuffer_info.yres;

				// dbg(9, "frame_width_px1=%d frame_height_px1=%d vid_width=%d vid_height=%d\n", (int)frame_width_px1, (int)frame_height_px1, (int)vid_width ,(int)vid_height);

				int downscale = 0;
				// check if we need to upscale or downscale
				if ((frame_width_px1 > (vid_width - 5)) || (frame_height_px1 > (vid_height - 5)))
				{
					// downscale to video size / or leave as is
					downscale = 1;
				}
				else
				{
					// upscale to video size
				}

				int buffer_size_in_bytes = y_layer_size + v_layer_size + u_layer_size;

				// dbg(9, "frame_width_px1=%d frame_width_px=%d frame_height_px1=%d\n", (int)frame_width_px1, (int)frame_width_px, (int)frame_height_px1);

				int horizontal_stride_pixels = 0;
				int horizontal_stride_pixels_half = 0;
				if (full_width > frame_width_px1)
				{
					horizontal_stride_pixels = full_width - frame_width_px1;
					horizontal_stride_pixels_half = horizontal_stride_pixels / 2;
				}


				uint8_t *bf_out_data = (uint8_t *)calloc(1, framebuffer_screensize);

				unsigned long int i, j;

				// dbg(9, "full_width=%f vid_width=%f full_height=%f vid_height=%f\n", (float)full_width, (float)vid_width, (float)full_height, (float)vid_height);
				float ww = (float)var_framebuffer_info.xres / (float)vid_width;
				float hh = (float)var_framebuffer_info.yres / (float)vid_height;
				// dbg(9, "video frame scale factor: full_width/vid_width=%f full_height/vid_height=%f\n", ww, hh);

				int horizontal_stride_pixels_half_resized = 0;
				if (ww > 0)
				{
					horizontal_stride_pixels_half_resized = 0 + (int)((float)horizontal_stride_pixels_half / ww);
					// dbg(9, "horizontal_stride_pixels_half_resized=%d\n", (int)horizontal_stride_pixels_half_resized);
				}

				int i_src;
				int j_src;
				int yx;
				int ux;
				int vx;


				int vid_height_needed = vid_height;
				if (hh > 0)
				{
					vid_height_needed = 0 + (int)((float)frame_height_px1 / hh);
					if (vid_height_needed > vid_height)
					{
						vid_height_needed = vid_height;
					}
				}
				// dbg(9, "vid_height_needed=%d vid_height=%d\n", (int)vid_height_needed, (int)vid_height);

				int vid_width_needed = vid_width;
				if (hh > 0)
				{
					vid_width_needed = 0 + (int)((float)frame_width_px1 / ww);
					if (vid_width_needed > vid_width)
					{
						vid_width_needed = vid_width;
					}
				}
				// dbg(9, "vid_width_needed=%d vid_width=%d\n", (int)vid_width_needed, (int)vid_width);







				if (downscale == 0)
				// if (((vid_height_needed + 10) < var_framebuffer_info.xres) && ((vid_height_needed + 10) < var_framebuffer_info.yres))
				{
					// scale image up to output size -----------------------------
					// scale image up to output size -----------------------------
					// scale image up to output size -----------------------------

					dbg(9, "scale image UP   ****\n");

					float ww2_upscale = (float)vid_width / (float)frame_width_px1;
					float hh2_upscale = (float)vid_height / (float)frame_height_px1;
					// dbg(9, "video frame scale factor2: ww=%f hh=%f\n", ww2_upscale, hh2_upscale);

					float factor2_upscale = hh2_upscale;
					if (ww2_upscale < hh2_upscale)
					{
						factor2_upscale = ww2_upscale;
					}
					// dbg(9, "factor2_upscale=%f\n", factor2_upscale);

					int scale_to_width_upscale = (int)((float)frame_width_px1 * factor2_upscale);
					int scale_to_height_upscale = (int)((float)frame_height_px1 * factor2_upscale);

					if (scale_to_width_upscale < 2)
					{
						scale_to_width_upscale = 2;
					}
					else if (scale_to_width_upscale > vid_width)
					{
						scale_to_width_upscale = vid_width;
					}

					if (scale_to_height_upscale < 2)
					{
						scale_to_height_upscale = 2;
					}
					else if (scale_to_height_upscale > vid_height)
					{
						scale_to_height_upscale = vid_height;
					}
					// dbg(9, "video frame scale to: ww=%d hh=%d\n", scale_to_width_upscale, scale_to_height_upscale);


					// convert to BGRA 1:1 size (from YUV)
					uint8_t *point = NULL;
					for (i = 0; i < frame_height_px1; i++)
					{
						i_src = i;
						for (j = 0; j < frame_width_px1; j++)
						{
							point = (uint8_t *) bf_out_data + 4 * ((i * (int)frame_width_px1) + j);
							j_src = j;

							yx = y[(i_src * abs(ystride)) + j_src];
							ux = u[((i_src / 2) * abs(ustride)) + (j_src / 2)];
							vx = v[((i_src / 2) * abs(vstride)) + (j_src / 2)];

							point[0] = YUV2B(yx, ux, vx); // B
							point[1] = YUV2G(yx, ux, vx); // G
							point[2] = YUV2R(yx, ux, vx); // R
							// point[3] = 0; // A
						}
					}


					uint8_t *bf_out_data_upscaled = (uint8_t *)calloc(1, framebuffer_screensize);
					memset(bf_out_data_upscaled, 0, framebuffer_screensize);

					// resize ---------------
					stbir_resize_uint8(bf_out_data, frame_width_px1, frame_height_px1, 0,
						bf_out_data_upscaled, scale_to_width_upscale, scale_to_height_upscale, (int)var_framebuffer_fix_info.line_length, 4);
					// dbg(9, "upscale res=%d\n", res_upscale);
					// resize ---------------

					if (bf_out_data != NULL)
					{
						free(bf_out_data);
						bf_out_data = NULL;
					}

					text_on_bgra_frame_xy(var_framebuffer_info.xres, var_framebuffer_info.yres,
						var_framebuffer_fix_info.line_length, bf_out_data_upscaled,
						10, var_framebuffer_info.yres - 50, status_line_1_str);
					text_on_bgra_frame_xy(var_framebuffer_info.xres, var_framebuffer_info.yres,
						var_framebuffer_fix_info.line_length, bf_out_data_upscaled,
						10, var_framebuffer_info.yres - 30, status_line_2_str);


					if (bf_out_data_upscaled != NULL)
					{
						sem_wait(&video_play_lock);
						fb_copy_frame_to_fb(bf_out_data_upscaled);
						sem_post(&video_play_lock);

						free(bf_out_data_upscaled);
						bf_out_data_upscaled = NULL;
					}
					// scale image up to output size -----------------------------
					// scale image up to output size -----------------------------
					// scale image up to output size -----------------------------
				}
				else
				{
					// scale image down to output size (or leave as is) ----------
					// scale image down to output size (or leave as is) ----------
					// scale image down to output size (or leave as is) ----------

					// dbg(9, "scale image DOWN ++++\n");
					memset(bf_out_data, 0, framebuffer_screensize);
					// dbg(9, "vid_width_needed=%d vid_height_needed=%d\n", (int)vid_width_needed, (int)vid_height_needed);



					float ww2_downscale = (float)vid_width / (float)frame_width_px1;
					float hh2_downscale = (float)vid_height / (float)frame_height_px1;
					// dbg(9, "video frame scale factor2: ww=%f hh=%f\n", ww2_downscale, hh2_downscale);

					float factor2_downscale = hh2_downscale;
					if (ww2_downscale < hh2_downscale)
					{
						factor2_downscale = ww2_downscale;
					}
					// dbg(9, "factor2_downscale=%f\n", factor2_downscale);

					int scale_to_width_downscale = (int)((float)frame_width_px1 * factor2_downscale);
					int scale_to_height_downscale = (int)((float)frame_height_px1 * factor2_downscale);

					if (scale_to_width_downscale < 2)
					{
						scale_to_width_downscale = 2;
					}
					else if (scale_to_width_downscale > vid_width)
					{
						scale_to_width_downscale = vid_width;
					}

					if (scale_to_height_downscale < 2)
					{
						scale_to_height_downscale = 2;
					}
					else if (scale_to_height_downscale > vid_height)
					{
						scale_to_height_downscale = vid_height;
					}
					// dbg(9, "video frame scale to: ww=%d hh=%d\n", scale_to_width_downscale, scale_to_height_downscale);






					int offset_right_px = (int)(((float)vid_width - (float)scale_to_width_downscale) / 2.0f);
					int offset_down_px = (int)(((float)vid_height - (float)scale_to_height_downscale) / 2.0f);

					// downscale and convert to BGRA in 1 step
					for (i = 0; i < scale_to_height_downscale; ++i)
					{
						i_src = (int)((float)i / factor2_downscale);
						for (j = 0; j < scale_to_width_downscale; ++j)
						{
							uint8_t *point = (uint8_t *) bf_out_data + 4 * //  '4 *'  ->  to get it in bytes (because 4 bytes per pixel)
							(
								((i + offset_down_px) * (int)var_framebuffer_fix_info.line_length / 4) + j + offset_right_px // in pixels
							);

							j_src = (int)((float)j / factor2_downscale);

							yx = y[(i_src * abs(ystride)) + j_src];
							ux = u[((i_src / 2) * abs(ustride)) + (j_src / 2)];
							vx = v[((i_src / 2) * abs(vstride)) + (j_src / 2)];

							point[0] = YUV2B(yx, ux, vx); // B
							point[1] = YUV2G(yx, ux, vx); // G
							point[2] = YUV2R(yx, ux, vx); // R
							// point[3] = 0; // A
						}
					}

					text_on_bgra_frame_xy(var_framebuffer_info.xres, var_framebuffer_info.yres,
						var_framebuffer_fix_info.line_length, bf_out_data,
						10, var_framebuffer_info.yres - 50, status_line_1_str);
					text_on_bgra_frame_xy(var_framebuffer_info.xres, var_framebuffer_info.yres,
						var_framebuffer_fix_info.line_length, bf_out_data,
						10, var_framebuffer_info.yres - 30, status_line_2_str);

					if (bf_out_data != NULL)
					{
						sem_wait(&video_play_lock);
						fb_copy_frame_to_fb(bf_out_data);
						sem_post(&video_play_lock);

						free(bf_out_data);
						bf_out_data = NULL;
					}
					// scale image down to output size (or leave as is) ----------
					// scale image down to output size (or leave as is) ----------
					// scale image down to output size (or leave as is) ----------
				}

	if (y)
	{
		free((void *)y);
	}

	if (u)
	{
		free((void *)u);
	}

	if (v)
	{
		free((void *)v);
	}


	dec_video_t_counter();
	pthread_exit(0);
}


// ---- DEBUG ----
static struct timeval tm_incoming_video_frames;
int first_incoming_video_frame = 1;
// ---- DEBUG ----


static void t_toxav_receive_video_frame_cb(ToxAV *av, uint32_t friend_number,
        uint16_t width, uint16_t height,
        uint8_t const *y, uint8_t const *u, uint8_t const *v,
        int32_t ystride, int32_t ustride, int32_t vstride,
        void *user_data)
{

	// ---- DEBUG ----
	if (first_incoming_video_frame == 0)
	{
		unsigned long long timspan_in_ms = 99999;
		timspan_in_ms = __utimer_stop(&tm_incoming_video_frames, "=== Video frame incoming every === :", 1);

		if ((timspan_in_ms > 0) && (timspan_in_ms < 99999))
		{
			global_video_in_fps = (int)(1000 / timspan_in_ms);
		}
		else
		{
			global_video_in_fps = 0;
		}

		update_fps_counter++;
		if (update_fps_counter > update_fps_every)
		{
			update_fps_counter = 0;
			update_status_line_1_text();
		}
	}
	else
	{
		first_incoming_video_frame = 0;
	}
	__utimer_start(&tm_incoming_video_frames);
	// ---- DEBUG ----



#ifdef HAVE_FRAMEBUFFER

	if (global_video_active == 1)
	{
		if (friend_to_send_video_to == friend_number)
		{
			if (global_framebuffer_device_fd != 0)
			{
				toggle_display_frame++;
				if (toggle_display_frame < SHOW_EVERY_X_TH_VIDEO_FRAME)
				{
					// dbg(9, "skipping video frame ...\n");
					return;
				}
				else
				{
					   toggle_display_frame = 0;
				}


				sem_wait(&video_play_lock);

				video__width = width;
				video__height = height;
				video__y = y;
				video__u = u;
				video__v = v;
				video__ystride = ystride;
				video__ustride = ustride;
				video__vstride = vstride;

				pthread_t video_play_thread;

				if (get_video_t_counter() <= MAX_VIDEO_PLAY_THREADS)
				{
					inc_video_t_counter();
					if (pthread_create(&video_play_thread, NULL, video_play, NULL))
					{
						dec_video_t_counter();
						dbg(0, "error creating video play thread\n");
					}
					else
					{
						// dbg(0, "creating video play thread #%d\n", get_video_t_counter());
						if (pthread_detach(video_play_thread))
						{
							dbg(0, "error detaching video play thread\n");
						}
						// zzzzzz
						yieldcpu(1);
					}
				}
				else
				{
					// dbg(1, "more than %d video play threads already\n", (int)MAX_VIDEO_PLAY_THREADS);
				}

				sem_post(&video_play_lock);
			}
		}
		else
		{
			// wrong friend
		}
	}
	else
	{
	}

#endif
}

void set_av_video_frame()
{
    vpx_img_alloc(&input, VPX_IMG_FMT_I420, video_width, video_height, 1);
    av_video_frame.y = input.planes[0]; /**< Y (Luminance) plane and  VPX_PLANE_PACKED */
    av_video_frame.u = input.planes[1]; /**< U (Chroma) plane */
    av_video_frame.v = input.planes[2]; /**< V (Chroma) plane */
    av_video_frame.w = input.d_w;
    av_video_frame.h = input.d_h;
	//av_video_frame.bit_depth = input.bit_depth;

    dbg(2,"ToxVideo:av_video_frame set\n");
}

void fb_copy_frame_to_fb(void* videoframe)
{
	if (framebuffer_mappedmem != NULL)
	{
		memcpy(framebuffer_mappedmem, videoframe, framebuffer_screensize);
	}
}

void fb_fill_black()
{
	if (framebuffer_mappedmem != NULL)
	{
		memset(framebuffer_mappedmem, 0x0, framebuffer_screensize);
	}
}


void fb_fill_xxx()
{
	if (framebuffer_mappedmem != NULL)
	{
		memset(framebuffer_mappedmem, 0xa3, framebuffer_screensize);
	}
}



void *video_record(void *dummy)
{
	TOXAV_ERR_SEND_FRAME error = 0;

	toxcam_av_video_frame *av_video_frame_copy = (toxcam_av_video_frame *)dummy;

	toxav_video_send_frame(mytox_av, friend_to_send_video_to, av_video_frame_copy->w, av_video_frame_copy->h,
		   av_video_frame_copy->y, av_video_frame_copy->u, av_video_frame_copy->v, &error);

	free(av_video_frame_copy->y);
	// free(av_video_frame_copy->u); // --> all in one buffer!!
	// free(av_video_frame_copy->v); // --> all in one buffer!!

	if (error)
	{
		if (error == TOXAV_ERR_SEND_FRAME_SYNC)
		{
			//debug_notice("uToxVideo:\tVid Frame sync error: w=%u h=%u\n", av_video_frame.w,
			//			 av_video_frame.h);
			dbg(0, "TOXAV_ERR_SEND_FRAME_SYNC\n");
		}
		else if (error == TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED)
		{
			//debug_error("uToxVideo:\tToxAV disagrees with our AV state for friend %lu, self %u, friend %u\n",
			//	i, friend[i].call_state_self, friend[i].call_state_friend);
			dbg(0, "TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED\n");
		}
		else
		{
			//debug_error("uToxVideo:\ttoxav_send_video error friend: %i error: %u\n",
			//			friend[i].number, error);
			dbg(0, "ToxVideo:toxav_send_video error %u\n", error);

			// *TODO* if these keep piling up --> just disconnect the call!!
			// *TODO* if these keep piling up --> just disconnect the call!!
			// *TODO* if these keep piling up --> just disconnect the call!!
		}
	}

	dec_video_trec_counter();
}



// ---- DEBUG ----
static struct timeval tm_outgoing_video_frames;
int first_outgoing_video_frame = 1;
// ---- DEBUG ----


void *thread_av(void *data)
{
	ToxAV *av = (ToxAV *) data;

	pthread_t id = pthread_self();
	pthread_mutex_t av_thread_lock;

#if 1
	// ------ thread priority ------
	struct sched_param param;
	int policy;
	int s;
	display_thread_sched_attr("Scheduler attributes of [1]: video iterate thread");
	get_policy('o', &policy);
	param.sched_priority = strtol("0", NULL, 0);
	// ****** // s = pthread_setschedparam(pthread_self(), policy, &param);
	// if (s != 0)
	// {
	// 	dbg(0, "Scheduler attributes of [2]: error setting scheduling attributes of video iterate thread\n");
	// }
	// else
	{
	}
	display_thread_sched_attr("Scheduler attributes of [3]: video iterate thread");
	// ------ thread priority ------
#endif

	if (pthread_mutex_init(&av_thread_lock, NULL) != 0)
	{
		dbg(0, "Error creating av_thread_lock\n");
	}
	else
	{
		dbg(2, "av_thread_lock created successfully\n");
	}

	dbg(2, "AV Thread #%d: starting\n", (int) id);

	if (video_call_enabled == 1)
	{
		global_framebuffer_device_fd = 0;

#ifdef HAVE_FRAMEBUFFER

		if ((global_framebuffer_device_fd = open(framebuffer_device, O_RDWR)) < 0)
		{
			dbg(0, "error opening Framebuffer device: %s\n", framebuffer_device);
		}
		else
		{
			dbg(2, "The Framebuffer device opened: %d\n", (int)global_framebuffer_device_fd);
		}

		// Get variable screen information
		if (ioctl(global_framebuffer_device_fd, FBIOGET_VSCREENINFO, &var_framebuffer_info))
		{
			dbg(0, "Error reading Framebuffer info\n");
		}

		dbg(2, "Framebuffer info %dx%d, %d bpp\n",  var_framebuffer_info.xres, var_framebuffer_info.yres, var_framebuffer_info.bits_per_pixel);

		// Get fixed screen information
		if (ioctl(global_framebuffer_device_fd, FBIOGET_FSCREENINFO, &var_framebuffer_fix_info))
		{
			dbg(0, "Error reading Framebuffer fixed information\n");
		}

		// map framebuffer to user memory
		framebuffer_screensize = (size_t)var_framebuffer_fix_info.smem_len;
		dbg(9, "framebuffer_screensize=%d\n", (int)framebuffer_screensize);

		framebuffer_mappedmem = NULL;
		framebuffer_mappedmem = (char*)mmap(NULL,
                    (size_t)framebuffer_screensize,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    global_framebuffer_device_fd, 0);

		if (framebuffer_mappedmem == NULL)
		{
			dbg(0, "Failed to mmap Framebuffer\n");
		}
		else
		{
			dbg(2, "mmap Framebuffer: %p\n", framebuffer_mappedmem);
		}

#endif

#ifdef HAVE_LIBAO

		libao_channels = 1;
		libao_sampling_rate = 48000;


		// initialize sound output via libao ------------------
		ao_initialize();
		_ao_default_driver = ao_default_driver_id();
		memset(&_ao_format, 0, sizeof(_ao_format));
		_ao_format.bits = 16;
		_ao_format.channels = (int)libao_channels;
		_ao_format.rate = (int)libao_sampling_rate;
		_ao_format.byte_format = AO_FMT_LITTLE;

		_ao_device = ao_open_live(_ao_default_driver, &_ao_format, NULL /* no options */);

		if (_ao_device == NULL)
		{
			dbg(0, "Error opening sound output device\n");
		}
		else
		{
		}
		// initialize sound output via libao ------------------
#endif


#ifdef HAVE_PORTAUDIO
			PaError err;
			err = Pa_Initialize();

			if( err != paNoError )
			{
				dbg(0, "Error opening sound output device\n");
			}

			libao_channels = 1;
			libao_sampling_rate = 48000;

			portaudio_out_rb = ringbuf_new((size_t) 1024 * 300); // 300kbytes ring buffer

			if (portaudio_out_rb == NULL)
			{
				dbg(0, "Error creating ringbuffer\n");
			}

#endif



		// --------------- start up the camera ---------------
		// --------------- start up the camera ---------------
		global_cam_device_fd = init_cam();
		dbg(2, "AV Thread #%d: init cam\n", (int) id);
		set_av_video_frame();
		// start streaming
		v4l_startread();
		// --------------- start up the camera ---------------
		// --------------- start up the camera ---------------

	}



	// --- ok, here camera and screen is ready to go
	// --- now show QR code
	stop_endless_loading();
	yieldcpu(700); // TODO: wait for qr-code file to be created (it is done in background!)
	show_tox_id_qrcode();


	// only now start accepting calls
	accepting_calls = 1;
	dbg(2, "--- accepting calls NOW ---\n");



    while (toxav_iterate_thread_stop != 1)
	{
		if (global_video_active == 1)
		{
			// pthread_mutex_lock(&av_thread_lock);



// ----------------- for sending video -----------------
// ----------------- for sending video -----------------
// ----------------- for sending video -----------------



			// dbg(9, "AV Thread #%d:get frame\n", (int) id);

            // capturing is enabled, capture frames
            int r = v4l_getframe(av_video_frame.y, av_video_frame.u, av_video_frame.v,
					av_video_frame.w, av_video_frame.h);

			if (r == 1)
			{

				if (global_send_first_frame > 0)
				{
					black_yuf_frame_xy();
					global_send_first_frame--;
				}

				// "0" -> [48]
				// "9" -> [57]
				// ":" -> [58]

				char* date_time_str = get_current_time_date_formatted();
				if (date_time_str)
				{

					text_on_yuf_frame_xy(10, 10, date_time_str);
					free(date_time_str);
				}

				blinking_dot_on_frame_xy(10, 30, &global_blink_state);

				if (friend_to_send_video_to != -1)
				{
					// dbg(9, "AV Thread #%d:send frame to friend num=%d\n", (int) id, (int)friend_to_send_video_to);


					// ---- DEBUG ----
					unsigned long long timspan_in_ms = 99999;
					if (first_outgoing_video_frame == 0)
					{
						timspan_in_ms = __utimer_stop(&tm_outgoing_video_frames, "sending video frame every:", 1);
						if (timspan_in_ms > DEFAULT_FPS_SLEEP_MS)
						{
							default_fps_sleep_corrected = (int)((long)DEFAULT_FPS_SLEEP_MS - ((long)timspan_in_ms - (long)DEFAULT_FPS_SLEEP_MS));
							if (default_fps_sleep_corrected < 0)
							{
								// dbg(9, "sending video frame: sleep 0ms\n");
								default_fps_sleep_corrected = 0;
							}
						}
						else
						{
							default_fps_sleep_corrected = DEFAULT_FPS_SLEEP_MS;
							// dbg(9, "sending video frame: sleep %d ms\n", (int)default_fps_sleep_corrected);
						}
					}
					else
					{
						first_outgoing_video_frame = 0;
					}
					__utimer_start(&tm_outgoing_video_frames);
					// ---- DEBUG ----


					if (global_show_fps_on_video == 1)
					{
						if ((timspan_in_ms > 0) && (timspan_in_ms < 99999))
						{
							char fps_str[1000];
							CLEAR(fps_str);
							snprintf(fps_str, sizeof(fps_str), "fps: %d", (int)(1000 / timspan_in_ms));
							text_on_yuf_frame_xy(50, 30, fps_str);
						}
						else
						{
							text_on_yuf_frame_xy(50, 30, "fps: --");
						}
					}

					if ((timspan_in_ms > 0) && (timspan_in_ms < 99999))
					{
						global_video_out_fps = (int)(1000 / timspan_in_ms);
					}
					else
					{
						global_video_out_fps = 0;
					}

					update_fps_counter++;
					if (update_fps_counter > update_fps_every)
					{
						update_fps_counter = 0;
						update_status_line_1_text();
					}


#if 1
					TOXAV_ERR_SEND_FRAME error = 0;
					toxav_video_send_frame(av, friend_to_send_video_to, av_video_frame.w, av_video_frame.h,
						   av_video_frame.y, av_video_frame.u, av_video_frame.v, &error);

					if (error)
					{
						if (error == TOXAV_ERR_SEND_FRAME_SYNC)
						{
							//debug_notice("uToxVideo:\tVid Frame sync error: w=%u h=%u\n", av_video_frame.w,
							//			 av_video_frame.h);
							dbg(0, "TOXAV_ERR_SEND_FRAME_SYNC\n");
						}
						else if (error == TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED)
						{
							//debug_error("uToxVideo:\tToxAV disagrees with our AV state for friend %lu, self %u, friend %u\n",
							//	i, friend[i].call_state_self, friend[i].call_state_friend);
							dbg(0, "TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED\n");
						}
						else
						{
							//debug_error("uToxVideo:\ttoxav_send_video error friend: %i error: %u\n",
							//			friend[i].number, error);
							dbg(0, "ToxVideo:toxav_send_video error %u\n", error);

							// *TODO* if these keep piling up --> just disconnect the call!!
							// *TODO* if these keep piling up --> just disconnect the call!!
							// *TODO* if these keep piling up --> just disconnect the call!!
						}
					}
#else

				// ---------------------------------------
				// TODO: threading here crashes :-(
				//       check me
				// ---------------------------------------

				pthread_t video_record_thread;

				if (get_video_trec_counter() <= MAX_VIDEO_RECORD_THREADS)
				{

					toxcam_av_video_frame av_video_frame_copy;
					size_t video_frame_size_bytes = (size_t)((av_video_frame.w * av_video_frame.h) * (3 / 2)); // TODO: stride!!!

					av_video_frame_copy.y = (uint8_t *)calloc(1, video_frame_size_bytes);
					av_video_frame_copy.u = (uint8_t *)(av_video_frame_copy.y + (av_video_frame.w * av_video_frame.h));
					av_video_frame_copy.v = (uint8_t *)(av_video_frame_copy.u + ((av_video_frame.w * av_video_frame.h) / 4));
					av_video_frame_copy.w = av_video_frame.w;
					av_video_frame_copy.h = av_video_frame.h;
					memcpy(av_video_frame_copy.y, av_video_frame.y, video_frame_size_bytes);

					inc_video_trec_counter();
					if (pthread_create(&video_record_thread, NULL, video_record, (void *)&av_video_frame_copy))
					{
						free(av_video_frame_copy.y);
						// free(av_video_frame_copy.u); // --> all in one buffer!!
						// free(av_video_frame_copy.v); // --> all in one buffer!!

						dec_video_trec_counter();
						dbg(0, "error creating video record thread\n");
					}
					else
					{
						dbg(0, "creating video record thread #%d\n", get_video_t_counter());
						if (pthread_detach(video_record_thread))
						{
							dbg(0, "error detaching video record thread\n");
						}
						// zzzzzz
						yieldcpu(1);
					}
				}
				else
				{
					dbg(1, "more than %d video record threads already\n", (int)MAX_VIDEO_RECORD_THREADS);
				}

				// ---------------------------------------
				// TODO: threading here crashes :-(
				//       check me
				// ---------------------------------------

#endif

				}

            }
			else if (r == -1)
			{
                // debug_error("uToxVideo:\tErr... something really bad happened trying to get this frame, I'm just going "
                //            "to plots now!\n");
                //video_device_stop();
                //close_video_device(video_device);
				// dbg(0, "ToxVideo:something really bad happened trying to get this frame\n");
            }

            // pthread_mutex_unlock(&av_thread_lock);

            yieldcpu(default_fps_sleep_corrected); /* ~25 frames per second */
            // yieldcpu(80); /* ~12 frames per second */
            // yieldcpu(40); /* 60fps = 16.666ms || 25 fps = 40ms || the data quality is SO much better at 25... */


// ----------------- for sending video -----------------
// ----------------- for sending video -----------------
// ----------------- for sending video -----------------

		}
		else
		{
			yieldcpu(100);
		}
    }


	if (video_call_enabled == 1)
	{
		// end streaming
		v4l_endread();
	}

	dbg(2, "ToxVideo:Clean thread exit!\n");
}


void *thread_video_av(void *data)
{
	ToxAV *av = (ToxAV *) data;

	pthread_t id = pthread_self();
	// pthread_mutex_t av_thread_lock;

	//if (pthread_mutex_init(&av_thread_lock, NULL) != 0)
	//{
	//	dbg(0, "Error creating video av_thread_lock\n");
	//}
	//else
	//{
	//	dbg(2, "av_thread_lock video created successfully\n");
	//}

	dbg(2, "AV video Thread #%d: starting\n", (int) id);

#if 1
	// ------ thread priority ------
	struct sched_param param;
	int policy;
	int s;
	display_thread_sched_attr("Scheduler attributes of [1]: video thread");
	get_policy('o', &policy);
	param.sched_priority = strtol("0", NULL, 0);
	s = pthread_setschedparam(pthread_self(), policy, &param);
	if (s != 0)
	{
		dbg(0, "Scheduler attributes of [2]: error setting scheduling attributes of video thread\n");
	}
	else
	{
	}
	display_thread_sched_attr("Scheduler attributes of [3]: video thread");
	// ------ thread priority ------
#endif

	while (toxav_video_thread_stop != 1)
	{
		// pthread_mutex_lock(&av_thread_lock);
		toxav_iterate(av);
		// dbg(9, "AV video Thread #%d running ...\n", (int) id);
		// pthread_mutex_unlock(&av_thread_lock);
		usleep(toxav_iteration_interval(av) * 1000);
	}

	dbg(2, "ToxVideo:Clean video thread exit!\n");
}


void av_local_disconnect(ToxAV *av, uint32_t num)
{
	int really_in_call = 0;
	
	if (global_video_active != 0)
	{
		really_in_call = 1;
	}
	
	dbg(9, "av_local_disconnect\n");
	TOXAV_ERR_CALL_CONTROL error = 0;
	toxav_call_control(av, num, TOXAV_CALL_CONTROL_CANCEL, &error);
	global_video_active = 0;

	dbg(9, "av_local_disconnect: global_video_active=%d\n", global_video_active);

	global_send_first_frame = 0;
	friend_to_send_video_to = -1;

	if (really_in_call == 1)
	{
		show_tox_id_qrcode();
	}
}


// ------------------ Tox AV stuff --------------------
// ------------------ Tox AV stuff --------------------
// ------------------ Tox AV stuff --------------------



// ------------------ YUV420 overlay hack -------------
// ------------------ YUV420 overlay hack -------------
// ------------------ YUV420 overlay hack -------------




/**
 * 8x8 monochrome bitmap fonts for rendering
 * Author: Daniel Hepper <daniel@hepper.net>
 *
 * License: Public Domain
 *
 * Based on:
 * // Summary: font8x8.h
 * // 8x8 monochrome bitmap fonts for rendering
 * //
 * // Author:
 * //     Marcel Sondaar
 * //     International Business Machines (public domain VGA fonts)
 * //
 * // License:
 * //     Public Domain
 *
 * Fetched from: http://dimensionalrift.homelinux.net/combuster/mos3/?p=viewsource&file=/modules/gfx/font8_8.asm
 **/

// Constant: font8x8_basic
// Contains an 8x8 font map for unicode points U+0000 - U+007F (basic latin)
char font8x8_basic[128][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0000 (nul)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0001
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0002
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0003
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0004
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0005
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0006
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0007
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0008
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0009
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000A
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000B
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000C
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000D
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000E
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0010
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0011
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0012
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0013
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0014
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0015
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0016
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0017
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0018
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0019
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001A
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001B
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001C
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001D
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001E
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0020 (space)
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},   // U+0021 (!)
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0022 (")
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},   // U+0023 (#)
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},   // U+0024 ($)
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},   // U+0025 (%)
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},   // U+0026 (&)
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0027 (')
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},   // U+0028 (()
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},   // U+0029 ())
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},   // U+002A (*)
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},   // U+002B (+)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},   // U+002C (,)
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},   // U+002D (-)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},   // U+002E (.)
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},   // U+002F (/)
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},   // U+0030 (0)
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},   // U+0031 (1)
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},   // U+0032 (2)
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},   // U+0033 (3)
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},   // U+0034 (4)
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},   // U+0035 (5)
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},   // U+0036 (6)
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},   // U+0037 (7)
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+0038 (8)
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},   // U+0039 (9)
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},   // U+003A (:)
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},   // U+003B (//)
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},   // U+003C (<)
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},   // U+003D (=)
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},   // U+003E (>)
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},   // U+003F (?)
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},   // U+0040 (@)
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},   // U+0041 (A)
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},   // U+0042 (B)
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},   // U+0043 (C)
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},   // U+0044 (D)
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},   // U+0045 (E)
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},   // U+0046 (F)
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},   // U+0047 (G)
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},   // U+0048 (H)
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0049 (I)
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},   // U+004A (J)
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},   // U+004B (K)
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},   // U+004C (L)
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},   // U+004D (M)
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},   // U+004E (N)
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},   // U+004F (O)
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},   // U+0050 (P)
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},   // U+0051 (Q)
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},   // U+0052 (R)
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},   // U+0053 (S)
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0054 (T)
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},   // U+0055 (U)
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   // U+0056 (V)
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},   // U+0057 (W)
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},   // U+0058 (X)
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},   // U+0059 (Y)
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},   // U+005A (Z)
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},   // U+005B ([)
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},   // U+005C (\)
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},   // U+005D (])
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},   // U+005E (^)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},   // U+005F (_)
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0060 (`)
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},   // U+0061 (a)
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},   // U+0062 (b)
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},   // U+0063 (c)
    { 0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},   // U+0064 (d)
    { 0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},   // U+0065 (e)
    { 0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},   // U+0066 (f)
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+0067 (g)
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},   // U+0068 (h)
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0069 (i)
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},   // U+006A (j)
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},   // U+006B (k)
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+006C (l)
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},   // U+006D (m)
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},   // U+006E (n)
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},   // U+006F (o)
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},   // U+0070 (p)
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},   // U+0071 (q)
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},   // U+0072 (r)
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},   // U+0073 (s)
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},   // U+0074 (t)
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},   // U+0075 (u)
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   // U+0076 (v)
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},   // U+0077 (w)
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},   // U+0078 (x)
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+0079 (y)
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},   // U+007A (z)
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},   // U+007B ({)
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},   // U+007C (|)
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},   // U+007D (})
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+007E (~)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}    // U+007F
};



// "0" -> [48]
// "9" -> [57]
// ":" -> [58]


void print_font_char(int start_x_pix, int start_y_pix, int font_char_num, uint8_t col_value)
{
	int font_w = 8;
	int font_h = 8;

	uint8_t *y_plane = av_video_frame.y;
	// uint8_t col_value = 0; // black
	char *bitmap = font8x8_basic[font_char_num];

	int k;
	int j;
	int offset = 0;
	int set = 0;

	for (k=0;k<font_h;k++)
	{
		y_plane = av_video_frame.y + ((start_y_pix + k) * av_video_frame.w);
		y_plane = y_plane + start_x_pix;
		for (j=0;j<font_w;j++)
		{
			set = bitmap[k] & 1 << j;
			if (set)
			{
				*y_plane = col_value; // set luma value
			}
			y_plane = y_plane + 1;
		}
	}

}

void black_yuf_frame_xy()
{
	const uint8_t r = 0;
	const uint8_t g = 0;
	const uint8_t b = 0;
	left_top_bar_into_yuv_frame(0, 0, av_video_frame.w, av_video_frame.h, r, g, b);
}

void blinking_dot_on_frame_xy(int start_x_pix, int start_y_pix, int* state)
{
	uint8_t r;
	uint8_t g;
	uint8_t b;

	if (*state == 0)
	{
		*state = 1;
		r = 255;
		g = 0;
		b = 0;
		left_top_bar_into_yuv_frame(start_x_pix, start_y_pix, 30, 30, r, g, b);
	}
	else if (*state == 1)
	{
		r = 255;
		g = 255;
		b = 0;
		*state = 2;
		left_top_bar_into_yuv_frame(start_x_pix, start_y_pix, 30, 30, r, g, b);
	}
	else
	{
		r = 0;
		g = 255;
		b = 0;
		*state = 0;
		left_top_bar_into_yuv_frame(start_x_pix, start_y_pix, 30, 30, r, g, b);
	}
}


void set_color_in_yuv_frame_xy(uint8_t *yuv_frame, int px_x, int px_y, int frame_w, int frame_h, uint8_t r, uint8_t g, uint8_t b)
{
	int size_total = frame_w * frame_h;

	uint8_t y;
	uint8_t u;
	uint8_t v;

	rbg_to_yuv(r, g, b, &y, &u, &v);

	yuv_frame[px_y * frame_w + px_x] = y;
	yuv_frame[(px_y / 2) * (frame_w / 2) + (px_x / 2) + size_total] = u;
	yuv_frame[(px_y / 2) * (frame_w / 2) + (px_x / 2) + size_total + (size_total / 4)] = v;
}

void rbg_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t *y, uint8_t *u, uint8_t *v)
{
	*y = RGB2Y(r, g, b);
	*u = RGB2U(r, g, b);
	*v = RGB2V(r, g, b);
}



void set_color_in_bgra_frame_xy(int fb_xres, int fb_yres, int fb_line_bytes, uint8_t *fb_buf, int px_x, int px_y, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *plane = fb_buf;
    plane = plane + (px_x * 4) + (px_y * fb_line_bytes);
	*plane = b; // b
	plane++;
	*plane = g; // g
	plane++;
	*plane = r; // r
	plane++;
	*plane = 0; // a
	// plane++;

	// size_t location = px_x * 4 + px_y * fb_line_bytes;
	// fb_buf[location] = b;
	// fb_buf[(location + 1)] = g;
	// fb_buf[(location + 2)] = r;
	// fb_buf[(location + 3)] = 0; // a,  No transparency
}


void left_top_bar_into_bgra_frame(int fb_xres, int fb_yres, int fb_line_bytes, uint8_t *fb_buf, int bar_start_x_pix, int bar_start_y_pix, int bar_w_pix, int bar_h_pix, uint8_t r, uint8_t g, uint8_t b)
{
	int k;
	int j;

	for (k=0;k<bar_h_pix;k++)
	{
		for (j=0;j<bar_w_pix;j++)
		{
			set_color_in_bgra_frame_xy(fb_xres, fb_yres, fb_line_bytes, fb_buf,
				(bar_start_x_pix + j), (bar_start_y_pix + k), r, g, b);
		}
	}
}


void print_font_char_rgba(int fb_xres, int fb_yres, int fb_line_bytes, uint8_t *fb_buf,
int start_x_pix, int start_y_pix, int font_char_num, uint8_t r, uint8_t g, uint8_t b)
{
	int font_w = 8;
	int font_h = 8;

	uint8_t *plane = fb_buf;
	char *bitmap = font8x8_basic[font_char_num];

	int k;
	int j;
	int offset = 0;
	int set = 0;

	for (k=0;k<font_h;k++)
	{
		plane = fb_buf + ((start_y_pix + k) * fb_line_bytes);
		plane = plane + start_x_pix * 4; // 4 bytes per pixel
		for (j=0;j<font_w;j++)
		{
			set = bitmap[k] & 1 << j;
			if (set)
			{
				*plane = b; // b
				plane++;
				*plane = g; // g
				plane++;
				*plane = r; // r
				plane++;
				*plane = 0; // a
				plane++;
			}
			else
			{
				plane = plane + 4;
			}
		}
	}

}


void text_on_bgra_frame_xy(int fb_xres, int fb_yres, int fb_line_bytes, uint8_t *fb_buf, int start_x_pix, int start_y_pix, const char* text)
{
	int carriage = 0;
	const int letter_width = 8;
	const int letter_spacing = 1;

	int block_needed_width = 2 + 2 + (strlen(text) * (letter_width + letter_spacing));
	left_top_bar_into_bgra_frame(fb_xres, fb_yres, fb_line_bytes, fb_buf, start_x_pix, start_y_pix, block_needed_width, 12, 255, 255, 255);

	int looper;

	for(looper=0;(int)looper < (int)strlen(text);looper++)
	{
		uint8_t c = text[looper];
		if ((c > 0) && (c < 127))
		{
			print_font_char_rgba(fb_xres, fb_yres, fb_line_bytes, fb_buf,
				(2 + start_x_pix + ((letter_width + letter_spacing) * carriage)),
				2 + start_y_pix,
				c, 0, 0, 0);
		}
		else
		{
			// leave a blank
		}
		carriage++;
	}
}






void text_on_yuf_frame_xy(int start_x_pix, int start_y_pix, const char* text)
{
	int carriage = 0;
	const int letter_width = 8;
	const int letter_spacing = 1;

	int block_needed_width = 2 + 2 + (strlen(text) * (letter_width + letter_spacing));
	left_top_bar_into_yuv_frame(start_x_pix, start_y_pix, block_needed_width, 12, 255, 255, 255);

	int looper;

	for(looper=0;(int)looper < (int)strlen(text);looper++)
	{
		uint8_t c = text[looper];
		if ((c > 0) && (c < 127))
		{
			print_font_char((2 + start_x_pix + ((letter_width + letter_spacing) * carriage)), 2 + start_y_pix, c, 0);
		}
		else
		{
			// leave a blank
		}
		carriage++;
	}
}

void left_top_bar_into_yuv_frame(int bar_start_x_pix, int bar_start_y_pix, int bar_w_pix, int bar_h_pix, uint8_t r, uint8_t g, uint8_t b)
{
	// int bar_width = bar_w_pix; // 150; // should be mulitple of 2 !!
	// int bar_height = bar_h_pix; // 20; // should be mulitple of 2 !!
	// int bar_start_x = bar_start_x_pix; // 10; // should be mulitple of 2 !! (zero is also ok)
	// int bar_start_y = bar_start_y_pix; // 10; // should be mulitple of 2 !! (zero is also ok)

	// uint8_t *y_plane = av_video_frame.y;

	int k;
	int j;
	// int offset = 0;

	for (k=0;k<bar_h_pix;k++)
	{
		// y_plane = av_video_frame.y + ((bar_start_y + k) * av_video_frame.w);
		// y_plane = y_plane + bar_start_x;
		for (j=0;j<bar_w_pix;j++)
		{
			// ******** // *y_plane = col_value; // luma value to 255 (white)
			set_color_in_yuv_frame_xy(av_video_frame.y, (bar_start_x_pix + j), (bar_start_y_pix + k),
				av_video_frame.w, av_video_frame.h, r, g, b);

			// y_plane = y_plane + 1;
		}
	}
}

// ------------------ YUV420 overlay hack -------------
// ------------------ YUV420 overlay hack -------------
// ------------------ YUV420 overlay hack -------------



static int get_policy(char p, int *policy)
{
   switch (p) {
   case 'f': *policy = SCHED_FIFO;     return 1;
   case 'r': *policy = SCHED_RR;       return 1;
   case 'b': *policy = SCHED_BATCH;    return 1;
   case 'o': *policy = SCHED_OTHER;    return 1;
   default:  return 0;
   }
}

static void display_sched_attr(char *msg, int policy, struct sched_param *param)
{
   dbg(9, "%s:policy=%s, priority=%d\n", msg,
		   (policy == SCHED_FIFO)  ? "SCHED_FIFO" :
		   (policy == SCHED_RR)    ? "SCHED_RR" :
		   (policy == SCHED_BATCH) ? "SCHED_BATCH" :
		   (policy == SCHED_OTHER) ? "SCHED_OTHER" :
		   "???",
		   param->sched_priority);
}

static void display_thread_sched_attr(char *msg)
{
	int policy, s;
	struct sched_param param;

	s = pthread_getschedparam(pthread_self(), &policy, &param);
	if (s != 0)
	{
		dbg(0, "error in display_thread_sched_attr\n");
	}

	display_sched_attr(msg, policy, &param);
}


// ------------------ alsa recording ------------------
// ------------------ alsa recording ------------------
// ------------------ alsa recording ------------------

#ifdef HAVE_ALSA_REC

void close_sound_device()
{
	if (have_input_sound_device == 1)
	{
		snd_pcm_close(audio_capture_handle);
	}
}

void init_sound_device()
{
		int i;
		int err;
		snd_pcm_hw_params_t *hw_params;

		have_input_sound_device = 1;

		// open in blocking mode for recording !!
		if ((err = snd_pcm_open(&audio_capture_handle, audio_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
			dbg(9, "record_device:cannot open audio device %s (%s)\n",
				 audio_device,
				 snd_strerror (err));
			//exit (1);
			have_input_sound_device = 0;
			return;
		}

		if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
			dbg(9, "record_device:cannot allocate hardware parameter structure (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params_any (audio_capture_handle, hw_params)) < 0) {
			dbg(9, "record_device:cannot initialize hardware parameter structure (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params_set_access (audio_capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			dbg(9, "record_device:cannot set access type (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params_set_format (audio_capture_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
			dbg(9, "record_device:cannot set sample format (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		unsigned int actualRate = DEFAULT_AUDIO_CAPTURE_SAMPLERATE;
		dbg(9, "record_device:sound: wanted audio rate:%d\n", actualRate);
		if ((err = snd_pcm_hw_params_set_rate_near (audio_capture_handle, hw_params, &actualRate, 0)) < 0) {
			dbg(9, "record_device:cannot set sample rate (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		dbg(9, "record_device:sound: got audio rate:%d\n", actualRate);

		// 1 -> mono, 2 -> stereo
		if ((err = snd_pcm_hw_params_set_channels (audio_capture_handle, hw_params, DEFAULT_AUDIO_CAPTURE_CHANNELS)) < 0) {
			dbg(9, "record_device:cannot set channel count (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params (audio_capture_handle, hw_params)) < 0) {
			dbg(9, "record_device:cannot set parameters (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		snd_pcm_hw_params_free (hw_params);

		if ((err = snd_pcm_prepare (audio_capture_handle)) < 0) {
			dbg(9, "record_device:cannot prepare audio interface for use (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}
}

void inc_audio_record_t_counter()
{
	sem_wait(&count_audio_record_threads);
	count_audio_record_threads_int++;
	sem_post(&count_audio_record_threads);
}

void dec_audio_record_t_counter()
{
	sem_wait(&count_audio_record_threads);
	count_audio_record_threads_int--;
	if (count_audio_record_threads_int < 0)
	{
		count_audio_record_threads_int = 0;
	}
	sem_post(&count_audio_record_threads);
}

int get_audio_record_t_counter()
{
	sem_wait(&count_audio_record_threads);
	int ret = count_audio_record_threads_int;
	sem_post(&count_audio_record_threads);
	return ret;
}

// r -u /dev/fb0 -j 640 -k 480
void *audio_record__(void *buf_pointer)
{
	if ((friend_to_send_video_to >= 0) && (global_video_active == 1))
	{
#if 1
		// make a local copy
		int16_t *audio_buf_orig = (int16_t *)buf_pointer;
		size_t audio_record_bytes_ = AUDIO_RECORD_BUFFER_BYTES;
		// int16_t *audio_buf_ = (int16_t *)malloc(audio_record_bytes_);
		if (audio_buf_orig)
		{
			// memcpy(audio_buf_, audio_buf_orig, audio_record_bytes_);
			size_t sample_count = (size_t)((audio_record_bytes_ / 2) / DEFAULT_AUDIO_CAPTURE_CHANNELS);

			TOXAV_ERR_SEND_FRAME error;
			bool res = toxav_audio_send_frame(mytox_av, (uint32_t)friend_to_send_video_to, (const int16_t *)audio_buf_orig, sample_count,
				(uint8_t)DEFAULT_AUDIO_CAPTURE_CHANNELS, (uint32_t)DEFAULT_AUDIO_CAPTURE_SAMPLERATE, &error);
			// dbg(9, "audio_record:006 TOXAV_ERR_SEND_FRAME=%d res=%d\n", (int)error, (int)res);

			free(audio_buf_orig);
		}
#endif
	}

	dec_audio_record_t_counter();
	pthread_exit(0);
}


void *thread_record_alsa_audio(void *data)
{
	int i;
	int err;
	do_audio_recording = 1;
	// ToxAV *av = (ToxAV *) data;

	init_sound_device();

	// ------ thread priority ------
	struct sched_param param;
	int policy;
	int s;
	display_thread_sched_attr("Scheduler attributes of [1]: thread_record_alsa_audio");
	get_policy('r', &policy);
#if 0
	param.sched_priority = strtol("2", NULL, 0);
	s = pthread_setschedparam(pthread_self(), policy, &param);
	if (s != 0)
	{
		dbg(0, "Scheduler attributes of [2]: error setting scheduling attributes of thread_record_alsa_audio\n");
	}
	else
	{
	}
	display_thread_sched_attr("Scheduler attributes of [3]: thread_record_alsa_audio");
#endif
	// ------ thread priority ------

	while ((do_audio_recording == 1) && (have_input_sound_device == 1))
	{
#if 1
		if ((friend_to_send_video_to >= 0) && (global_video_active == 1))
		{

			if (have_input_sound_device == 1)
			{
				snd_pcm_reset(audio_capture_handle);

				int16_t *audio_buf_l = (int16_t *)calloc(1, (size_t)AUDIO_RECORD_BUFFER_BYTES);
				if ((err = snd_pcm_readi(audio_capture_handle, audio_buf_l, AUDIO_RECORD_BUFFER_FRAMES)) != AUDIO_RECORD_BUFFER_FRAMES)
				{
					dbg(1, "record_device:read from audio interface failed (err=%d) (%s)\n", (int)err, snd_strerror(err));
					free(audio_buf_l);

					if ((int)err == -11) // -> Resource temporarily unavailable
					{
						dbg(0, "play_device:yield a bit (2)\n");
						// zzzzzz
						yieldcpu(1);
					}

					close_sound_device();
					dbg(9, "record_device:close_sound_device\n");
					// zzzzzz
					yieldcpu(20);
					init_sound_device();
				}
				else
				{
					// dbg(1, "read from audio interface OK (frames=%d)\n", (int)err);

					pthread_t audio_record_thread__;
					if (get_audio_record_t_counter() <= MAX_ALSA_RECORD_THREADS)
					{
						inc_audio_record_t_counter();
						if (pthread_create(&audio_record_thread__, NULL, audio_record__, (void *)audio_buf_l))
						{
							dec_audio_record_t_counter();
							dbg(0, "error creating audio record thread\n");

							free(audio_buf_l);
						}
						else
						{
							// int pthread_setschedparam(audio_record_thread__, int policy, const struct sched_param *param);

							// pthread_setname_np(audio_record_thread__, "audio_rec_thd__");

							//dbg(0, "creating audio play thread #%d\n", get_audio_t_counter());
							if (pthread_detach(audio_record_thread__))
							{
								dbg(0, "error detaching audio record thread\n");
							}
						}
					}
					else
					{
						dbg(1, "more than %d audio record threads already\n", (int)MAX_ALSA_RECORD_THREADS);

						free(audio_buf_l);
					}
				}
			}
		}
		else
		{
#endif
			// sleep 0.5 seconds
			yieldcpu(500);
#if 1
		}
#endif
	}

	close_sound_device();
}

#endif

// ------------------ alsa recording ------------------
// ------------------ alsa recording ------------------
// ------------------ alsa recording ------------------


void call_entry_num(Tox *tox, int entry_num)
{
	if (accepting_calls == 1)
	{
		uint8_t *caller_toxid_bin = NULL;
		read_pubkey_from_file(&caller_toxid_bin, entry_num);

		if (caller_toxid_bin != NULL)
		{
			int64_t entry_num_friendnum = friend_number_for_entry(tox, caller_toxid_bin);

			if (entry_num_friendnum != -1)
			{
				if (accepting_calls == 1)
				{
					cmd_vcm(tox, entry_num_friendnum);
				}
			}

			free(caller_toxid_bin);
		}
	}
}

void toggle_speaker()
{
#ifdef HAVE_ALSA_PLAY

	if (speaker_out_num == 0)
	{
		speaker_out_num = 1;
	}
	else
	{
		speaker_out_num = 0;
	}

	dbg(9, "toggle_speaker:speaker_out_num=%d\n", speaker_out_num);

	sem_wait(&audio_play_lock);
	close_sound_play_device();

	// -- toggle alsa config with sudo command --
    char cmd_001[1000];
	CLEAR(cmd_001);
    snprintf(cmd_001, sizeof(cmd_001), "sudo ./toggle_alsa.sh %d", (int)speaker_out_num);
	dbg(9, "toggle_speaker:cmd=%s\n", cmd_001);
	if (system(cmd_001));
	// -- toggle alsa config with sudo command --

	yieldcpu(1000); // wait 1 second !!

	init_sound_play_device((int)libao_channels, (int)libao_sampling_rate);
	sem_post(&audio_play_lock);

	update_status_line_2_text();
	update_status_line_on_fb();

#endif
}

void toggle_quality()
{
	int vbr_new = DEFAULT_GLOBAL_VID_BITRATE;

	if (global_video_bit_rate == DEFAULT_GLOBAL_VID_BITRATE_NORMAL_QUALITY)
	{
		vbr_new = DEFAULT_GLOBAL_VID_BITRATE_HIGHER_QUALITY;
		dbg(2, "toggle_quality: HIGH\n");
		global__VP8E_SET_CPUUSED_VALUE = 10;
		global__VPX_END_USAGE = 3;
	}
	else
	{
		vbr_new = DEFAULT_GLOBAL_VID_BITRATE_NORMAL_QUALITY;
		dbg(2, "toggle_quality: normal\n");
		global__VP8E_SET_CPUUSED_VALUE = 16;
		global__VPX_END_USAGE = 2;
	}

	DEFAULT_GLOBAL_VID_BITRATE = (int32_t)vbr_new;
	global_video_bit_rate = DEFAULT_GLOBAL_VID_BITRATE;

	update_status_line_1_text();
	update_status_line_on_fb();

	if (mytox_av != NULL)
	{
		if (friend_to_send_video_to > -1)
		{
			toxav_bit_rate_set(mytox_av, friend_to_send_video_to, global_audio_bit_rate, global_video_bit_rate, NULL);
		}
	}
}

#ifdef HAVE_EXTERNAL_KEYS

void *thread_ext_keys(void *data)
{
	char buf[MAX_READ_FIFO_BUF];
	do_read_ext_keys = 1;
	int res = 0;

	Tox *tox = (Tox *)data;

	mkfifo(ext_keys_fifo, 0666);
	ext_keys_fd = open(ext_keys_fifo, O_RDONLY);

	while (do_read_ext_keys == 1)
	{
		res = read(ext_keys_fd, buf, MAX_READ_FIFO_BUF);
		if (res == 0)
		{
			// dbg(9, "ExtKeys: reopening FIFO for reading\n");
			close(ext_keys_fd);
			mkfifo(ext_keys_fifo, 0666);
			yieldcpu(50);
			ext_keys_fd = open(ext_keys_fifo, O_RDONLY);
		}
		else
		{
			dbg(9, "ExtKeys: received: %s\n", buf);

			if (strncmp((char*)buf, "call:1", strlen((char*)"call:1")) == 0)
			{
				dbg(2, "ExtKeys: CALL:1\n");
				call_entry_num(tox, 1);
			}
			else if (strncmp((char*)buf, "call:2", strlen((char*)"call:2")) == 0)
			{
				dbg(2, "ExtKeys: CALL:2\n");
				call_entry_num(tox, 2);
			}
			else if (strncmp((char*)buf, "hangup:", strlen((char*)"hangup:")) == 0)
			{
				dbg(2, "ExtKeys: HANGUP:\n");
				disconnect_all_calls(tox);
			}
			else if (strncmp((char*)buf, "toggle_quality:", strlen((char*)"toggle_quality:")) == 0)
			{
				dbg(2, "ExtKeys: TOGGLE QUALITY:\n");
				toggle_quality();
			}
			else if (strncmp((char*)buf, "toggle_speaker:", strlen((char*)"toggle_speaker:")) == 0)
			{
				dbg(2, "ExtKeys: TOGGLE SPEAKER:\n");
				toggle_speaker();
			}
		}
	}

    close(ext_keys_fd);

}

#endif



#ifdef HAVE_ALSA_PLAY

void close_sound_play_device()
{
	dbg(0, "ALSA:015\n");

	if (have_output_sound_device == 1)
	{
		snd_pcm_close(audio_play_handle);
	}

	dbg(0, "ALSA:016\n");

}

void init_sound_play_device(int channels, int sample_rate)
{
		dbg(0, "ALSA:002\n");

		int i;
		int err;
		snd_pcm_hw_params_t *hw_params;

		have_output_sound_device = 1;

		// open in blocking mode for playing
		if ((err = snd_pcm_open(&audio_play_handle, audio_play_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
			dbg(9, "play_device:cannot open audio play device %s (%s)\n",
				 audio_play_device,
				 snd_strerror (err));
			//exit (1);
			have_output_sound_device = 0;
			return;
		}

		dbg(0, "ALSA:003\n");


		if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
			dbg(9, "play_device:cannot allocate hardware parameter structure (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params_any (audio_play_handle, hw_params)) < 0) {
			dbg(9, "play_device:cannot initialize hardware parameter structure (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params_set_access (audio_play_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			dbg(9, "play_device:cannot set access type (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params_set_format (audio_play_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
			dbg(9, "play_device:cannot set sample format (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		unsigned int actualRate = sample_rate;
		dbg(9, "play_device:sound: wanted audio rate:%d\n", actualRate);
		if ((err = snd_pcm_hw_params_set_rate_near (audio_play_handle, hw_params, &actualRate, 0)) < 0) {
			dbg(9, "play_device:cannot set sample rate (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		dbg(9, "sound: got audio rate:%d\n", actualRate);

		// 1 -> mono, 2 -> stereo
		if ((err = snd_pcm_hw_params_set_channels (audio_play_handle, hw_params, channels)) < 0) {
			dbg(9, "play_device:cannot set channel count (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		if ((err = snd_pcm_hw_params (audio_play_handle, hw_params)) < 0) {
			dbg(9, "play_device:cannot set parameters (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		snd_pcm_hw_params_free(hw_params);





		snd_pcm_sw_params_t *swparams;
		snd_pcm_sw_params_alloca(&swparams);


        /* get the current swparams */
        err = snd_pcm_sw_params_current(audio_play_handle, swparams);
        if (err < 0) {
                dbg(9, "play_device:Unable to determine current swparams for playback: %s\n", snd_strerror(err));
        }

		snd_pcm_uframes_t val;

		err = snd_pcm_sw_params_get_start_threshold(swparams, &val);
		dbg(9, "play_device:get_start_threshold:%d\n", (int)val);
		err = snd_pcm_sw_params_get_silence_threshold(swparams, &val);
		dbg(9, "play_device:get_silence_threshold:%d\n", (int)val);

        /* start the transfer when the buffer is almost full: */
        /* (buffer_size / avail_min) * avail_min */
        err = snd_pcm_sw_params_set_start_threshold(audio_play_handle, swparams, (snd_pcm_uframes_t)ALSA_AUDIO_PLAY_START_THRESHOLD);
        if (err < 0)
		{
			dbg(9, "play_device:Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
		}

        err = snd_pcm_sw_params_set_silence_threshold(audio_play_handle, swparams, (snd_pcm_uframes_t)ALSA_AUDIO_PLAY_SILENCE_THRESHOLD);
        if (err < 0)
		{
			dbg(9, "play_device:Unable to set silence threshold mode for playback: %s\n", snd_strerror(err));
		}

		err = snd_pcm_sw_params_get_start_threshold(swparams, &val);
		dbg(9, "play_device:get_start_threshold (after):%d\n", (int)val);
		err = snd_pcm_sw_params_get_silence_threshold(swparams, &val);
		dbg(9, "play_device:get_silence_threshold (after):%d\n", (int)val);


        /* write the parameters to the playback device */
        err = snd_pcm_sw_params(audio_play_handle, swparams);
        if (err < 0) {
                dbg(9, "play_device:Unable to set sw params for playback: %s\n", snd_strerror(err));
        }

		if ((err = snd_pcm_prepare (audio_play_handle)) < 0) {
			dbg(9, "play_device:cannot prepare audio interface for use (%s)\n",
				 snd_strerror (err));
			//exit (1);
		}

		dbg(0, "ALSA:009\n");

}


static int sound_play_xrun_recovery(snd_pcm_t *handle, int err, int channels, int sample_rate)
{
		// dbg(9, "play_device:stream recovery ...\n");

		// dbg(0, "ALSA:0010\n");


        if (err == -EPIPE)
		{
				// dbg(9, "play_device:under-run ...\n");
				/* under-run */
                err = snd_pcm_prepare(handle);
                if (err < 0)
				{
                        dbg(9, "play_device:underrun!: %s\n", snd_strerror(err));
						// zzzzzz
						// yieldcpu(20);
						close_sound_play_device();
						// dbg(9, "play_device:close_sound_play_device\n");
						// zzzzzz
						// yieldcpu(20);
						init_sound_play_device(channels, sample_rate);
						// dbg(9, "play_device:init_sound_play_device\n");
				}
                return 0;
        }
		else if (err == -ESTRPIPE)
		{
				// dbg(9, "play_device:snd_pcm_resume ...\n");
                while ((err = snd_pcm_resume(handle)) == -EAGAIN)
				{
                        sleep(1); /* wait until the suspend flag is released */
						// dbg(9, "play_device:snd_pcm_resume ... SLEEP\n");
						// yieldcpu(100);
						//if ((err = snd_pcm_resume(handle)) == -ENOSYS)
						//{
						//	dbg(9, "play_device:ENOSYS\n");
						//
						//	err = -1;
						//	break;
						//}
				}
				// dbg(9, "play_device:snd_pcm_resume ... READY\n");

                if (err < 0)
				{
						// dbg(9, "play_device:snd_pcm_prepare\n");
                        err = snd_pcm_prepare(handle);
                        if (err < 0)
						{
                                dbg(9, "play_device:suspend!: %s\n", snd_strerror(err));
								// zzzzzz
								// yieldcpu(20);
								close_sound_play_device();
								// dbg(9, "play_device:close_sound_play_device\n");
								// zzzzzz
								// yieldcpu(20);
								init_sound_play_device(channels, sample_rate);
								// dbg(9, "play_device:init_sound_play_device\n");
						}
                }

				dbg(0, "ALSA:011\n");

                return 0;
        }

		// dbg(9, "play_device:stream recovery ... READY?\n");

		dbg(0, "ALSA:012\n");

        return err;
}

#endif








void sigint_handler(int signo)
{
    if (signo == SIGINT)
    {
    	printf("received SIGINT, pid=%d\n", getpid());
    	tox_loop_running = 0;

#if 0
		kill_all_file_transfers(tox);
		close_cam();
		toxav_kill(mytox_av);
		tox_kill(tox);

		if (logfile)
		{
			fclose(logfile);
			logfile = NULL;
		}

		exit(77);
#endif
    }
}

int main(int argc, char *argv[])
{
	// don't accept calls until video device is ready
	accepting_calls = 0;

	show_endless_loading();

	global_want_restart = 0;
	global_video_active = 0;
	global_send_first_frame = 0;

	global_show_fps_on_video = 0;

	incoming_filetransfers = 0;


	// valid audio bitrates: [ bit_rate < 6 || bit_rate > 510 ]
	global_audio_bit_rate = DEFAULT_GLOBAL_AUD_BITRATE;
	global_video_bit_rate = DEFAULT_GLOBAL_VID_BITRATE;

	default_fps_sleep_corrected = DEFAULT_FPS_SLEEP_MS;

	video_high = 0;

    logfile = fopen(log_filename, "wb");
    setvbuf(logfile, NULL, _IONBF, 0);

	v4l2_device = malloc(400);
	memset(v4l2_device, 0, 400);
	snprintf(v4l2_device, 399, "%s", "/dev/video0");

	framebuffer_device = malloc(400);
	memset(framebuffer_device, 0, 400);
	snprintf(framebuffer_device, 399, "%s", "/dev/fb0");

	int aflag = 0;
	char *cvalue = NULL;
	int index;
	int opt;

   const char     *short_opt = "hvd:t23b:fu:j:k:";
   struct option   long_opt[] =
   {
      {"help",          no_argument,       NULL, 'h'},
      {"version",       no_argument,       NULL, 'v'},
      {"videodevice",   required_argument, NULL, 'd'},
      {NULL,            0,                 NULL,  0 }
   };

  while((opt = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1)
  {
    switch (opt)
      {
      case -1:       /* no more arguments */
      case 0:        /* long options toggles */
        break;
      case 'a':
        aflag = 1;
        break;
      case '2':
        switch_nodelist_2 = 1;
        break;
      case '3':
        switch_nodelist_2 = 2;
        break;
      case 't':
        switch_tcponly = 1;
        break;
      case 'f':
        video_high = 1;
        break;
      case 'd':
        snprintf(v4l2_device, 399, "%s", optarg);
        dbg(3, "Using Videodevice: %s\n", v4l2_device);
        break;
      case 'u':
        snprintf(framebuffer_device, 399, "%s", optarg);
        dbg(3, "Using Framebufferdevice: %s\n", framebuffer_device);
        break;
      case 'b':
        DEFAULT_GLOBAL_VID_BITRATE = (uint32_t)atoi(optarg);
        dbg(3, "Using Videobitrate: %d\n", (int)DEFAULT_GLOBAL_VID_BITRATE);
        global_video_bit_rate = DEFAULT_GLOBAL_VID_BITRATE;
        break;
      case 'j':
        vid_width = (int)atoi(optarg);
        dbg(3, "Using Wall Pixels X: %d\n", (int)vid_width);
        break;
      case 'k':
        vid_height = (int)atoi(optarg);
        dbg(3, "Using Wall Pixels Y: %d\n", (int)vid_height);
        break;
      case 'v':
         printf("ToxBlinkenwall version: %s\n", global_version_string);
            if (logfile)
            {
                fclose(logfile);
                logfile = NULL;
            }
         return(0);
        break;

      case 'h':
         printf("Usage: %s [OPTIONS]\n", argv[0]);
         printf("  -d, --videodevice devicefile         file\n");
         printf("  -u devicefile                        file\n");
         printf("  -b bitrate                           video bitrate in kbit/s\n");
         printf("  -j pixels                            wall x pixels\n");
         printf("  -k pixels                            wall y pixels\n");
         printf("  -f                                   use 720p video mode\n");
         printf("  -t,                                  tcp only mode\n");
         printf("  -2,                                  use alternate bootnode list\n");
         printf("  -3,                                  use only nodes.tox.chat as bootnode\n");
         printf("  -v, --version                        show version\n");
         printf("  -h, --help                           print this help and exit\n");
         printf("\n");
            if (logfile)
            {
                fclose(logfile);
                logfile = NULL;
            }
         return(0);

      case ':':
      case '?':
         fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
            if (logfile)
            {
                fclose(logfile);
                logfile = NULL;
            }
         return(-2);

      default:
         fprintf(stderr, "%s: invalid option -- %c\n", argv[0], opt);
         fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
            if (logfile)
            {
                fclose(logfile);
                logfile = NULL;
            }
         return(-2);
      }
  }


    CLEAR(status_line_1_str);
    CLEAR(status_line_2_str);
	global_video_in_fps = 0;
	global_video_out_fps = 0;

	// snprintf(status_line_1_str, sizeof(status_line_1_str), "V: I/O/OB %d/%d/%d", 0, 0, (int)global_video_bit_rate);
	// snprintf(status_line_2_str, sizeof(status_line_2_str), "A:     OB %d", (int)global_audio_bit_rate);
	update_status_line_1_text();
	update_status_line_2_text();

	// -- set priority of process with sudo command --
#if 0
	pid_t my_pid = getpid();
    char cmd_001[1000];
	CLEAR(cmd_001);
    snprintf(cmd_001, sizeof(cmd_001), "sudo chrt -f -p 1 %d", (int)my_pid);
	if (system(cmd_001));
	dbg(9, "set priority of process with sudo command [no output]:%s\n", cmd_001);
#endif
	// -- set priority of process with sudo command --


	pthread_setname_np(pthread_self(), "main thread");


	dbg(9, "global__MAX_DECODE_TIME_US=%d\n", global__MAX_DECODE_TIME_US); // 0, 1, 1000000  (0 BEST, 1 REALTIME, 1000000 GOOD)
	/*
	VPX_DL_REALTIME       (1)
	deadline parameter analogous to VPx REALTIME mode.
	VPX_DL_GOOD_QUALITY   (1000000)
	deadline parameter analogous to VPx GOOD QUALITY mode.
	VPX_DL_BEST_QUALITY   (0)
	deadline parameter analogous to VPx BEST QUALITY mode.
	*/

	dbg(9, "global__VP8E_SET_CPUUSED_VALUE=%d\n", global__VP8E_SET_CPUUSED_VALUE); // -16 .. 16 (-16 hard, 16 fast)
	dbg(9, "global__VPX_END_USAGE=%d\n", global__VPX_END_USAGE); // 0 .. 3
	/*
		0 -> VPX_VBR Variable Bit Rate (VBR) mode
		1 -> VPX_CBR Constant Bit Rate (CBR) mode
		2 -> VPX_CQ  Constrained Quality (CQ)  mode
		3 -> VPX_Q   Constant Quality (Q) mode
	*/

	global__MAX_DECODE_TIME_US = 1;
	global__VP8E_SET_CPUUSED_VALUE = 16;
	global__VPX_END_USAGE = 2;
	global__VPX_KF_MAX_DIST = 12;
	global__VPX_G_LAG_IN_FRAMES = 0;


	// ------ thread priority ------
	struct sched_param param;
	int policy;
	int s;
	display_thread_sched_attr("Scheduler attributes of [1]: main thread");
	get_policy('o', &policy);
	param.sched_priority = strtol("0", NULL, 0);
#if 0
	s = pthread_setschedparam(pthread_self(), policy, &param);
	if (s != 0)
	{
		dbg(0, "Scheduler attributes of [2]: error setting scheduling attributes of main thread\n");
	}
	else
	{
	}
#endif
	display_thread_sched_attr("Scheduler attributes of [3]: main thread");
	// ------ thread priority ------


#ifdef HAVE_LIBAO
	count_audio_play_threads_int = 0;

	if (sem_init(&count_audio_play_threads, 0, 1))
	{
		dbg(0, "Error in sem_init for count_audio_play_threads\n");
	}

	if (sem_init(&audio_play_lock, 0, 1))
	{
		dbg(0, "Error in sem_init for audio_play_lock\n");
	}
#endif

#ifdef HAVE_ALSA_PLAY
	init_sound_play_device(1, 48000);
	count_audio_play_threads_int = 0;

	if (sem_init(&count_audio_play_threads, 0, 1))
	{
		dbg(0, "Error in sem_init for count_audio_play_threads\n");
	}

	if (sem_init(&audio_play_lock, 0, 1))
	{
		dbg(0, "Error in sem_init for audio_play_lock\n");
	}
#endif


	if (sem_init(&video_play_lock, 0, 1))
	{
		dbg(0, "Error in sem_init for video_play_lock\n");
	}

	count_video_play_threads_int = 0;

	if (sem_init(&count_video_play_threads, 0, 1))
	{
		dbg(0, "Error in sem_init for count_video_play_threads\n");
	}

	count_video_record_threads_int = 0;

	if (sem_init(&count_video_record_threads, 0, 1))
	{
		dbg(0, "Error in sem_init for count_video_record_threads\n");
	}


	Tox *tox = create_tox();
	global_start_time = time(NULL);

    const char *name = "ToxBlinkenwall";
    tox_self_set_name(tox, (uint8_t *)name, strlen(name), NULL);

    const char *status_message = "Metalab Blinkenwall";
    tox_self_set_status_message(tox, (uint8_t *)status_message, strlen(status_message), NULL);

    Friends.max_idx = 0;


    bootstrap(tox);


    print_tox_id(tox);
	delete_qrcode_generate_touchfile();
	create_tox_id_qrcode(tox);

    // init callbacks ----------------------------------
    tox_callback_friend_request(tox, friend_request_cb);
    tox_callback_friend_message(tox, friend_message_cb);
    tox_callback_friend_connection_status(tox, friendlist_onConnectionChange);
	tox_callback_friend_status(tox, on_tox_friend_status);

    tox_callback_self_connection_status(tox, self_connection_status_cb);

    tox_callback_file_chunk_request(tox, on_file_chunk_request);
    tox_callback_file_recv_control(tox, on_file_control);
    tox_callback_file_recv(tox, on_file_recv);
    tox_callback_file_recv_chunk(tox, on_file_recv_chunk);
    // init callbacks ----------------------------------


    update_savedata_file(tox);
    load_friendlist(tox);

    char path[300];
    snprintf(path, sizeof(path), "%s", my_avatar_filename);
    int len = strlen(path) - 1;
    avatar_set(tox, path, len);

	long long unsigned int cur_time = time(NULL);
	uint8_t off = 1;
	long long loop_counter = 0;
	while (1)
	{
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox) * 1000);
        if (tox_self_get_connection_status(tox) && off)
		{
            dbg(2, "Tox online, took %llu seconds\n", time(NULL) - cur_time);
            off = 0;
			break;
        }
        c_sleep(20);

		loop_counter++;
		if (loop_counter > (50 * 20))
		{
			loop_counter = 0;
			// if not yet online, bootstrap every 20 seconds
			dbg(2, "Tox NOT online yet, bootstrapping again\n");
			bootstrap(tox);
		}
    }

	// -- here tox node is online, but video is not yet ready!!

    TOXAV_ERR_NEW rc;
	dbg(2, "new Tox AV\n");
    mytox_av = toxav_new(tox, &rc);
	if (rc != TOXAV_ERR_NEW_OK)
	{
		dbg(0, "Error at toxav_new: %d\n", rc);
	}

	CallControl mytox_CC;
	memset(&mytox_CC, 0, sizeof(CallControl));

    // init AV callbacks -------------------------------
    toxav_callback_call(mytox_av, t_toxav_call_cb, &mytox_CC);
    toxav_callback_call_state(mytox_av, t_toxav_call_state_cb, &mytox_CC);
    toxav_callback_bit_rate_status(mytox_av, t_toxav_bit_rate_status_cb, &mytox_CC);
    toxav_callback_video_receive_frame(mytox_av, t_toxav_receive_video_frame_cb, &mytox_CC);
    toxav_callback_audio_receive_frame(mytox_av, t_toxav_receive_audio_frame_cb, &mytox_CC);
    // init AV callbacks -------------------------------


	// start toxav thread ------------------------------
	pthread_t tid[4]; // 0 -> toxav_iterate thread, 1 -> video send thread, 2 -> audio recording thread, 3 -> read keys from pipe

	// start toxav thread ------------------------------
	toxav_iterate_thread_stop = 0;
    if (pthread_create(&(tid[0]), NULL, thread_av, (void *)mytox_av) != 0)
	{
        dbg(0, "AV iterate Thread create failed\n");
	}
	else
	{
		pthread_setname_np(tid[0], "thread_av");
        dbg(2, "AV iterate Thread successfully created\n");
	}

	toxav_video_thread_stop = 0;
    if (pthread_create(&(tid[1]), NULL, thread_video_av, (void *)mytox_av) != 0)
	{
        dbg(0, "AV video Thread create failed\n");
	}
	else
	{
		pthread_setname_np(tid[1], "thread_video_av");
        dbg(2, "AV video Thread successfully created\n");
	}
	// start toxav thread ------------------------------

	// start audio recoding stuff ----------------------
#ifdef HAVE_ALSA_REC

	count_audio_record_threads_int = 0;

	if (sem_init(&count_audio_record_threads, 0, 1))
	{
		dbg(0, "Error in sem_init for count_audio_record_threads\n");
	}

    if (pthread_create(&(tid[2]), NULL, thread_record_alsa_audio, (void *)mytox_av) != 0)
	{
        dbg(0, "AV Audio Thread create failed\n");
	}
	else
	{
		pthread_setname_np(tid[2], "t_rec_alsa_audio");
        dbg(2, "AV Audio Thread successfully created\n");
	}

#endif
	// start audio recoding stuff ----------------------


#ifdef HAVE_EXTERNAL_KEYS
    if (pthread_create(&(tid[3]), NULL, thread_ext_keys, (void *)tox) != 0)
	{
        dbg(0, "ExtKeys Thread create failed\n");
	}
	else
	{
		pthread_setname_np(tid[3], "t_ext_keys");
        dbg(2, "ExtKeys Thread successfully created\n");
	}
#endif


	// ------ thread priority ------
	yieldcpu(800); // wait for other thread to start up, and set their priority
	// struct sched_param param;
	// int policy;
	// int s;
	display_thread_sched_attr("Scheduler attributes of [1]: main thread");
	get_policy('o', &policy);
	param.sched_priority = strtol("0", NULL, 0);
	s = pthread_setschedparam(pthread_self(), policy, &param);
	if (s != 0)
	{
		dbg(0, "Scheduler attributes of [2]:error setting scheduling attributes of main thread\n");
	}
	else
	{
	}
	display_thread_sched_attr("Scheduler attributes of [3]: main thread");
	// ------ thread priority ------


    tox_loop_running = 1;
    signal(SIGINT, sigint_handler);

    while (tox_loop_running)
    {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox) * 1000);

		if (global_want_restart == 1)
		{
			// need to restart me!
			break;
		}
		else
		{
			if ((global_qrcode_was_updated == 1) && (global_is_qrcode_showing_on_screen == 1))
			{
				dbg(2, "QR code changed, waiting for update ...\n");

				if (is_qrcode_generated() == 1)
				{
					dbg(2, "update QR code on screen\n");
					global_qrcode_was_updated = 0;
					delete_qrcode_generate_touchfile();
					// fb_fill_black();
					if (global_video_active == 0)
					{
						show_tox_id_qrcode();
					}
				}
			}

		}
    }

#ifdef HAVE_ALSA_REC
	do_audio_recording = 0;
	yieldcpu(800);
#endif

    kill_all_file_transfers(tox);
    close_cam();
    toxav_kill(mytox_av);
    tox_kill(tox);

#ifdef HAVE_LIBAO
	sem_destroy(&count_audio_play_threads);
	sem_destroy(&audio_play_lock);
#endif

sem_destroy(&count_video_play_threads);
sem_destroy(&video_play_lock);

#ifdef HAVE_ALSA_PLAY
	close_sound_play_device();
	sem_destroy(&count_audio_play_threads);
	sem_destroy(&audio_play_lock);
#endif

#ifdef HAVE_ALSA_REC
	sem_destroy(&count_audio_record_threads);
#endif

#ifdef HAVE_EXTERNAL_KEYS
	do_read_ext_keys = 0;
	yieldcpu(10);
#endif


    if (logfile)
    {
        fclose(logfile);
        logfile = NULL;
    }

    return 0;
}


