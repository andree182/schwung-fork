/*
 * Fork MIDI Splitter & Router
 * Splits an input MIDI stream into multiple channels or pipe streams based on pitch thresholds.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#if defined(__aarch64__)
__asm__(".symver pthread_create, pthread_create@GLIBC_2.17");
__asm__(".symver pthread_join, pthread_join@GLIBC_2.17");
#endif

#define RING_BUFFER_SIZE 512

typedef struct {
    uint8_t data[3];
    uint8_t len;
} midi_msg_t;

typedef struct {
    midi_msg_t buffer[RING_BUFFER_SIZE];
    volatile int head;
    volatile int tail;
} ring_buffer_t;

static inline void ring_buffer_init(ring_buffer_t *rb) {
    rb->head = 0;
    rb->tail = 0;
}

static inline int ring_buffer_push(ring_buffer_t *rb, const uint8_t *msg, uint8_t len) {
    int next_head = (rb->head + 1) % RING_BUFFER_SIZE;
    if (next_head == rb->tail) {
        return 0; // Full
    }
    rb->buffer[rb->head].len = len;
    memcpy(rb->buffer[rb->head].data, msg, len);
    __sync_synchronize();
    rb->head = next_head;
    return 1;
}

static inline int ring_buffer_pop(ring_buffer_t *rb, uint8_t *out_msg, uint8_t *out_len) {
    if (rb->head == rb->tail) {
        return 0; // Empty
    }
    *out_len = rb->buffer[rb->tail].len;
    memcpy(out_msg, rb->buffer[rb->tail].data, *out_len);
    int next_tail = (rb->tail + 1) % RING_BUFFER_SIZE;
    __sync_synchronize();
    rb->tail = next_tail;
    return 1;
}

typedef enum {
    STATE_READ_LEN = 0,
    STATE_READ_DATA
} recv_state_t;

typedef struct {
    recv_state_t state;
    uint8_t msg_len;
    uint8_t bytes_read;
    uint8_t msg_data[3];
} fifo_parser_t;

typedef struct {
    int mode; // 0 = Splitter, 1 = Receiver
    int split_oct_1; // -1 if Off, else 0-9
    int split_oct_2; // -1 if Off, else 0-9
    int split_oct_3; // -1 if Off, else 0-9
    int transpose;
    int split1_chan; // absolute channel index 0-15
    int split2_chan; // absolute channel index 0-15
    int split3_chan; // absolute channel index 0-15

    char split1_path[256];
    char split2_path[256];
    char split3_path[256];
    char recv_path[256];

    char next_split1_path[256];
    char next_split2_path[256];
    char next_split3_path[256];
    char next_recv_path[256];
    int paths_dirty;

    pthread_mutex_t path_mutex;

    ring_buffer_t split1_ring_buf;
    ring_buffer_t split2_ring_buf;
    ring_buffer_t split3_ring_buf;
    ring_buffer_t recv_ring_buf;

    pthread_t io_thread;
    volatile int thread_running;

    int split1_fd;
    int split2_fd;
    int split3_fd;
    int recv_fd;

    uint8_t active_note_dest[16][128]; // Destination tracker to avoid stuck notes

    char pipe_1_select[64];
    char pipe_2_select[64];
    char pipe_3_select[64];
    char recv_pipe_select[64];
    char split_1_chan_str[64];
    char split_2_chan_str[64];
    char split_3_chan_str[64];

    char chain_params_json[16384];
    int chain_params_len;
} fork_instance_t;

static const host_api_v1_t *g_host = NULL;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void get_fifo_path(const char *name, char *buf, size_t buf_len) {
    if (name[0] == '/') {
        snprintf(buf, buf_len, "%s", name);
        return;
    }
    if (access("/data/UserData/schwung", F_OK) == 0) {
        snprintf(buf, buf_len, "/data/UserData/schwung/%s", name);
    } else if (access("/data/UserData", F_OK) == 0) {
        snprintf(buf, buf_len, "/data/UserData/%s", name);
    } else {
        snprintf(buf, buf_len, "./%s", name);
    }
}

static void resolve_pipe_path(const char *select_val, const char *default_name, char *out_path, size_t max_len) {
    char name[64];
    if (select_val && select_val[0] >= '1' && select_val[0] <= '8' && select_val[1] == '\0') {
        snprintf(name, sizeof(name), "midifork%c", select_val[0]);
    } else {
        snprintf(name, sizeof(name), "%s", default_name);
    }
    get_fifo_path(name, out_path, max_len);
}

static uint8_t adjust_channel(uint8_t status, int target_chan) {
    if (status >= 0xF0) {
        return status;
    }
    uint8_t type = status & 0xF0;
    return type | (uint8_t)(target_chan & 0x0F);
}

static int parse_split_octave(const char *val) {
    if (!val || strcmp(val, "off") == 0) return -1;
    if (strcmp(val, "C-1") == 0) return 0;
    if (strcmp(val, "C0") == 0) return 1;
    if (strcmp(val, "C1") == 0) return 2;
    if (strcmp(val, "C2") == 0) return 3;
    if (strcmp(val, "C3") == 0) return 4;
    if (strcmp(val, "C4") == 0) return 5;
    if (strcmp(val, "C5") == 0) return 6;
    if (strcmp(val, "C6") == 0) return 7;
    if (strcmp(val, "C7") == 0) return 8;
    if (strcmp(val, "C8") == 0) return 9;
    
    int n = atoi(val);
    if (n >= 0 && n <= 9) return n;
    return -1;
}

static const char* format_split_octave(int oct) {
    if (oct == 0) return "C-1";
    if (oct == 1) return "C0";
    if (oct == 2) return "C1";
    if (oct == 3) return "C2";
    if (oct == 4) return "C3";
    if (oct == 5) return "C4";
    if (oct == 6) return "C5";
    if (oct == 7) return "C6";
    if (oct == 8) return "C7";
    if (oct == 9) return "C8";
    return "off";
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    const char *pos, *colon, *end;
    int len;
    if (!json || !key || !out || out_len < 1) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return 0;
    colon = strchr(pos, ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;
    end = strchr(colon, '"');
    if (!end) return 0;
    len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len);
    out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char search[64];
    const char *pos, *colon;
    if (!json || !key || !out) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return 0;
    colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

static void cache_chain_params_from_module_json(fork_instance_t *inst, const char *module_dir) {
    char path[512];
    FILE *f;
    char *json = NULL;
    long size;
    const char *chain_params, *arr_start, *arr_end;
    int depth = 1;
    if (!inst || !module_dir || !module_dir[0]) return;
    snprintf(path, sizeof(path), "%s/module.json", module_dir);
    f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    size = ftell(f);
    if (size <= 0 || size > 300000) { fclose(f); return; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    json = (char *)malloc((size_t)size + 1);
    if (!json) { fclose(f); return; }
    if (fread(json, 1, (size_t)size, f) != (size_t)size) { free(json); fclose(f); return; }
    json[size] = '\0';
    fclose(f);

    chain_params = strstr(json, "\"chain_params\"");
    if (!chain_params) { free(json); return; }
    arr_start = strchr(chain_params, '[');
    if (!arr_start) { free(json); return; }
    arr_end = arr_start + 1;
    while (*arr_end && depth > 0) {
        if (*arr_end == '[') depth++;
        else if (*arr_end == ']') depth--;
        arr_end++;
    }
    if (depth == 0) {
        int len = (int)(arr_end - arr_start);
        if (len > 0 && len < (int)sizeof(inst->chain_params_json)) {
            memcpy(inst->chain_params_json, arr_start, (size_t)len);
            inst->chain_params_json[len] = '\0';
            inst->chain_params_len = len;
        }
    }
    free(json);
}

static void* io_thread_func(void *arg) {
    fork_instance_t *inst = (fork_instance_t *)arg;
    fifo_parser_t parser;
    memset(&parser, 0, sizeof(parser));
    parser.state = STATE_READ_LEN;

    char local_split1_path[256] = "";
    char local_split2_path[256] = "";
    char local_split3_path[256] = "";
    char local_recv_path[256] = "";
    int local_mode = -1;

    while (inst->thread_running) {
        pthread_mutex_lock(&inst->path_mutex);
        int paths_changed = inst->paths_dirty;
        if (paths_changed) {
            strcpy(local_split1_path, inst->next_split1_path);
            strcpy(local_split2_path, inst->next_split2_path);
            strcpy(local_split3_path, inst->next_split3_path);
            strcpy(local_recv_path, inst->next_recv_path);
            inst->paths_dirty = 0;
        }
        int mode_changed = (local_mode != inst->mode);
        if (mode_changed) {
            local_mode = inst->mode;
        }
        pthread_mutex_unlock(&inst->path_mutex);

        if (paths_changed || mode_changed) {
            if (inst->split1_fd >= 0) { close(inst->split1_fd); inst->split1_fd = -1; }
            if (inst->split2_fd >= 0) { close(inst->split2_fd); inst->split2_fd = -1; }
            if (inst->split3_fd >= 0) { close(inst->split3_fd); inst->split3_fd = -1; }
            if (inst->recv_fd >= 0) { close(inst->recv_fd); inst->recv_fd = -1; }
            memset(&parser, 0, sizeof(parser));
            parser.state = STATE_READ_LEN;

            strcpy(inst->split1_path, local_split1_path);
            strcpy(inst->split2_path, local_split2_path);
            strcpy(inst->split3_path, local_split3_path);
            strcpy(inst->recv_path, local_recv_path);
        }

        if (local_mode == 0) {
            // Splitter Mode
            if (inst->split_oct_1 >= 0 && strcmp(inst->pipe_1_select, "off") != 0) {
                if (inst->split1_fd < 0) {
                    mkfifo(inst->split1_path, 0666);
                    int fd = open(inst->split1_path, O_WRONLY | O_NONBLOCK);
                    if (fd >= 0) {
                        inst->split1_fd = fd;
                    }
                }
            } else {
                if (inst->split1_fd >= 0) { close(inst->split1_fd); inst->split1_fd = -1; }
            }

            if (inst->split_oct_2 >= 0 && strcmp(inst->pipe_2_select, "off") != 0) {
                if (inst->split2_fd < 0) {
                    mkfifo(inst->split2_path, 0666);
                    int fd = open(inst->split2_path, O_WRONLY | O_NONBLOCK);
                    if (fd >= 0) {
                        inst->split2_fd = fd;
                    }
                }
            } else {
                if (inst->split2_fd >= 0) { close(inst->split2_fd); inst->split2_fd = -1; }
            }

            if (inst->split_oct_3 >= 0 && strcmp(inst->pipe_3_select, "off") != 0) {
                if (inst->split3_fd < 0) {
                    mkfifo(inst->split3_path, 0666);
                    int fd = open(inst->split3_path, O_WRONLY | O_NONBLOCK);
                    if (fd >= 0) {
                        inst->split3_fd = fd;
                    }
                }
            } else {
                if (inst->split3_fd >= 0) { close(inst->split3_fd); inst->split3_fd = -1; }
            }

            uint8_t msg[3];
            uint8_t len;
            if (inst->split_oct_1 >= 0 && strcmp(inst->pipe_1_select, "off") != 0) {
                if (inst->split1_fd >= 0) {
                    while (ring_buffer_pop(&inst->split1_ring_buf, msg, &len)) {
                        uint8_t frame[4];
                        frame[0] = len;
                        memcpy(frame + 1, msg, len);
                        int written = write(inst->split1_fd, frame, len + 1);
                        if (written < 0) {
                            if (errno == EPIPE || errno == EBADF) {
                                close(inst->split1_fd);
                                inst->split1_fd = -1;
                                break;
                            }
                        }
                    }
                }
            } else {
                while (ring_buffer_pop(&inst->split1_ring_buf, msg, &len)) {
                    // Discard
                }
            }

            if (inst->split_oct_2 >= 0 && strcmp(inst->pipe_2_select, "off") != 0) {
                if (inst->split2_fd >= 0) {
                    while (ring_buffer_pop(&inst->split2_ring_buf, msg, &len)) {
                        uint8_t frame[4];
                        frame[0] = len;
                        memcpy(frame + 1, msg, len);
                        int written = write(inst->split2_fd, frame, len + 1);
                        if (written < 0) {
                            if (errno == EPIPE || errno == EBADF) {
                                close(inst->split2_fd);
                                inst->split2_fd = -1;
                                break;
                            }
                        }
                    }
                }
            } else {
                while (ring_buffer_pop(&inst->split2_ring_buf, msg, &len)) {
                    // Discard
                }
            }

            if (inst->split_oct_3 >= 0 && strcmp(inst->pipe_3_select, "off") != 0) {
                if (inst->split3_fd >= 0) {
                    while (ring_buffer_pop(&inst->split3_ring_buf, msg, &len)) {
                        uint8_t frame[4];
                        frame[0] = len;
                        memcpy(frame + 1, msg, len);
                        int written = write(inst->split3_fd, frame, len + 1);
                        if (written < 0) {
                            if (errno == EPIPE || errno == EBADF) {
                                close(inst->split3_fd);
                                inst->split3_fd = -1;
                                break;
                            }
                        }
                    }
                }
            } else {
                while (ring_buffer_pop(&inst->split3_ring_buf, msg, &len)) {
                    // Discard
                }
            }

        } else {
            // Receiver Mode
            if (inst->recv_fd < 0) {
                mkfifo(inst->recv_path, 0666);
                int fd = open(inst->recv_path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    inst->recv_fd = fd;
                }
            }

            if (inst->recv_fd >= 0) {
                uint8_t buf[128];
                while (1) {
                    int ret = read(inst->recv_fd, buf, sizeof(buf));
                    if (ret > 0) {
                        for (int i = 0; i < ret; i++) {
                            uint8_t byte = buf[i];
                            if (parser.state == STATE_READ_LEN) {
                                if (byte >= 1 && byte <= 3) {
                                    parser.msg_len = byte;
                                    parser.bytes_read = 0;
                                    parser.state = STATE_READ_DATA;
                                }
                            } else if (parser.state == STATE_READ_DATA) {
                                parser.msg_data[parser.bytes_read++] = byte;
                                if (parser.bytes_read == parser.msg_len) {
                                    ring_buffer_push(&inst->recv_ring_buf, parser.msg_data, parser.msg_len);
                                    parser.state = STATE_READ_LEN;
                                }
                            }
                        }
                    } else if (ret == 0) {
                        // EOF: Writer closed their end of the FIFO
                        close(inst->recv_fd);
                        inst->recv_fd = -1;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            close(inst->recv_fd);
                            inst->recv_fd = -1;
                        }
                        break;
                    }
                }
            }
        }

        usleep(1000);
    }

    return NULL;
}

static void* fork_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;
    fork_instance_t *inst = (fork_instance_t *)calloc(1, sizeof(fork_instance_t));
    if (!inst) return NULL;

    inst->mode = 0;
    inst->split_oct_1 = -1;
    inst->split_oct_2 = -1;
    inst->split_oct_3 = -1;
    inst->transpose = 0;

    inst->split1_chan = 0;
    inst->split2_chan = 1;
    inst->split3_chan = 2;

    strcpy(inst->pipe_1_select, "1");
    strcpy(inst->pipe_2_select, "2");
    strcpy(inst->pipe_3_select, "3");
    strcpy(inst->recv_pipe_select, "1");
    strcpy(inst->split_1_chan_str, "1");
    strcpy(inst->split_2_chan_str, "2");
    strcpy(inst->split_3_chan_str, "3");

    memset(inst->active_note_dest, 0xFF, sizeof(inst->active_note_dest));

    pthread_mutex_init(&inst->path_mutex, NULL);

    ring_buffer_init(&inst->split1_ring_buf);
    ring_buffer_init(&inst->split2_ring_buf);
    ring_buffer_init(&inst->split3_ring_buf);
    ring_buffer_init(&inst->recv_ring_buf);

    inst->split1_fd = -1;
    inst->split2_fd = -1;
    inst->split3_fd = -1;
    inst->recv_fd = -1;

    resolve_pipe_path(inst->pipe_1_select, "midifork1", inst->split1_path, sizeof(inst->split1_path));
    resolve_pipe_path(inst->pipe_2_select, "midifork2", inst->split2_path, sizeof(inst->split2_path));
    resolve_pipe_path(inst->pipe_3_select, "midifork3", inst->split3_path, sizeof(inst->split3_path));
    resolve_pipe_path(inst->recv_pipe_select, "midifork1", inst->recv_path, sizeof(inst->recv_path));

    strcpy(inst->next_split1_path, inst->split1_path);
    strcpy(inst->next_split2_path, inst->split2_path);
    strcpy(inst->next_split3_path, inst->split3_path);
    strcpy(inst->next_recv_path, inst->recv_path);

    signal(SIGPIPE, SIG_IGN);

    inst->thread_running = 1;
    if (pthread_create(&inst->io_thread, NULL, io_thread_func, inst) != 0) {
        inst->thread_running = 0;
    }

    cache_chain_params_from_module_json(inst, module_dir);

    return inst;
}

static void fork_destroy_instance(void *instance) {
    fork_instance_t *inst = (fork_instance_t *)instance;
    if (!inst) return;

    inst->thread_running = 0;
    pthread_join(inst->io_thread, NULL);

    if (inst->split1_fd >= 0) close(inst->split1_fd);
    if (inst->split2_fd >= 0) close(inst->split2_fd);
    if (inst->split3_fd >= 0) close(inst->split3_fd);
    if (inst->recv_fd >= 0) close(inst->recv_fd);

    pthread_mutex_destroy(&inst->path_mutex);
    free(inst);
}

static void fork_set_param(void *instance, const char *key, const char *val);
static int fork_get_param(void *instance, const char *key, char *buf, int buf_len);

static int fork_process_midi(void *instance,
                             const uint8_t *in_msg, int in_len,
                             uint8_t out_msgs[][3], int out_lens[],
                             int max_out) {
    (void)max_out;
    fork_instance_t *inst = (fork_instance_t *)instance;
    if (!inst || !in_msg || in_len < 1) return 0;

    uint8_t status = in_msg[0];
    uint8_t type = status & 0xF0;
    uint8_t orig_chan = status & 0x0F;

    if (inst->mode == 1) {
        return 0;
    }

    if ((type == 0x90 || type == 0x80 || type == 0xA0) && in_len >= 3) {
        uint8_t note = in_msg[1];
        uint8_t vel_or_press = in_msg[2];
        
        int s1 = (inst->split_oct_1 >= 0) ? (inst->split_oct_1 * 12) : -1;
        int s2 = (inst->split_oct_2 >= 0) ? (inst->split_oct_2 * 12) : -1;
        int s3 = (inst->split_oct_3 >= 0) ? (inst->split_oct_3 * 12) : -1;
        
        int dest = 0;
        
        if (type == 0x90 && vel_or_press > 0) {
            if (s1 >= 0 && note >= s1) {
                dest = 1;
            }
            if (s2 >= 0 && note >= s2) {
                dest = 2;
            }
            if (s3 >= 0 && note >= s3) {
                dest = 3;
            }
            inst->active_note_dest[orig_chan][note] = dest;
        } else {
            if (inst->active_note_dest[orig_chan][note] != 0xFF) {
                dest = inst->active_note_dest[orig_chan][note];
            } else {
                if (s1 >= 0 && note >= s1) {
                    dest = 1;
                }
                if (s2 >= 0 && note >= s2) {
                    dest = 2;
                }
                if (s3 >= 0 && note >= s3) {
                    dest = 3;
                }
            }
            if (type == 0x80 || (type == 0x90 && vel_or_press == 0)) {
                inst->active_note_dest[orig_chan][note] = 0xFF;
            }
        }

        if (dest == 0) {
            int new_note = note + inst->transpose * 12;
            new_note = clamp_int(new_note, 0, 127);
            out_msgs[0][0] = status;
            out_msgs[0][1] = (uint8_t)new_note;
            out_msgs[0][2] = vel_or_press;
            out_lens[0] = in_len;
            return 1;
        } else if (dest == 1) {
            int new_note = note;
            uint8_t out_msg[3];
            out_msg[0] = adjust_channel(status, inst->split1_chan);
            out_msg[1] = (uint8_t)new_note;
            out_msg[2] = vel_or_press;
            if (strcmp(inst->pipe_1_select, "off") == 0) {
                out_msgs[0][0] = out_msg[0];
                out_msgs[0][1] = out_msg[1];
                out_msgs[0][2] = out_msg[2];
                out_lens[0] = in_len;
                return 1;
            } else {
                ring_buffer_push(&inst->split1_ring_buf, out_msg, in_len);
                if (g_host && g_host->midi_inject_to_move && type == 0x90 && vel_or_press > 0) {
                    uint8_t target_chan = out_msg[0] & 0x0F;
                    uint8_t inject_pkt[4] = { 0x2B, (uint8_t)(0xB0 | target_chan), 119, 0 };
                    g_host->midi_inject_to_move(inject_pkt, 4);
                }
                return 0;
            }
        } else if (dest == 2) {
            int new_note = note;
            uint8_t out_msg[3];
            out_msg[0] = adjust_channel(status, inst->split2_chan);
            out_msg[1] = (uint8_t)new_note;
            out_msg[2] = vel_or_press;
            if (strcmp(inst->pipe_2_select, "off") == 0) {
                out_msgs[0][0] = out_msg[0];
                out_msgs[0][1] = out_msg[1];
                out_msgs[0][2] = out_msg[2];
                out_lens[0] = in_len;
                return 1;
            } else {
                ring_buffer_push(&inst->split2_ring_buf, out_msg, in_len);
                if (g_host && g_host->midi_inject_to_move && type == 0x90 && vel_or_press > 0) {
                    uint8_t target_chan = out_msg[0] & 0x0F;
                    uint8_t inject_pkt[4] = { 0x2B, (uint8_t)(0xB0 | target_chan), 119, 0 };
                    g_host->midi_inject_to_move(inject_pkt, 4);
                }
                return 0;
            }
        } else {
            int new_note = note;
            uint8_t out_msg[3];
            out_msg[0] = adjust_channel(status, inst->split3_chan);
            out_msg[1] = (uint8_t)new_note;
            out_msg[2] = vel_or_press;
            if (strcmp(inst->pipe_3_select, "off") == 0) {
                out_msgs[0][0] = out_msg[0];
                out_msgs[0][1] = out_msg[1];
                out_msgs[0][2] = out_msg[2];
                out_lens[0] = in_len;
                return 1;
            } else {
                ring_buffer_push(&inst->split3_ring_buf, out_msg, in_len);
                if (g_host && g_host->midi_inject_to_move && type == 0x90 && vel_or_press > 0) {
                    uint8_t target_chan = out_msg[0] & 0x0F;
                    uint8_t inject_pkt[4] = { 0x2B, (uint8_t)(0xB0 | target_chan), 119, 0 };
                    g_host->midi_inject_to_move(inject_pkt, 4);
                }
                return 0;
            }
        }
    }

    if (status >= 0x80 && status < 0xF8) {
        int out_count = 0;
        if (out_count < max_out) {
            memcpy(out_msgs[out_count], in_msg, in_len);
            out_lens[out_count] = in_len;
            out_count++;
        }
        
        int s1 = (inst->split_oct_1 >= 0) ? (inst->split_oct_1 * 12) : -1;
        int s2 = (inst->split_oct_2 >= 0) ? (inst->split_oct_2 * 12) : -1;
        int s3 = (inst->split_oct_3 >= 0) ? (inst->split_oct_3 * 12) : -1;
        
        if (s1 >= 0) {
            uint8_t out_msg[3];
            memcpy(out_msg, in_msg, in_len);
            out_msg[0] = adjust_channel(status, inst->split1_chan);
            if (strcmp(inst->pipe_1_select, "off") == 0) {
                if (out_count < max_out) {
                    memcpy(out_msgs[out_count], out_msg, in_len);
                    out_lens[out_count] = in_len;
                    out_count++;
                }
            } else {
                ring_buffer_push(&inst->split1_ring_buf, out_msg, in_len);
            }
        }
        if (s2 >= 0) {
            uint8_t out_msg[3];
            memcpy(out_msg, in_msg, in_len);
            out_msg[0] = adjust_channel(status, inst->split2_chan);
            if (strcmp(inst->pipe_2_select, "off") == 0) {
                if (out_count < max_out) {
                    memcpy(out_msgs[out_count], out_msg, in_len);
                    out_lens[out_count] = in_len;
                    out_count++;
                }
            } else {
                ring_buffer_push(&inst->split2_ring_buf, out_msg, in_len);
            }
        }
        if (s3 >= 0) {
            uint8_t out_msg[3];
            memcpy(out_msg, in_msg, in_len);
            out_msg[0] = adjust_channel(status, inst->split3_chan);
            if (strcmp(inst->pipe_3_select, "off") == 0) {
                if (out_count < max_out) {
                    memcpy(out_msgs[out_count], out_msg, in_len);
                    out_lens[out_count] = in_len;
                    out_count++;
                }
            } else {
                ring_buffer_push(&inst->split3_ring_buf, out_msg, in_len);
            }
        }
        return out_count;
    }

    if (status >= 0xF8) {
        memcpy(out_msgs[0], in_msg, in_len);
        out_lens[0] = in_len;
        return 1;
    }

    return 0;
}

static int fork_tick(void *instance, int frames, int sample_rate,
                     uint8_t out_msgs[][3], int out_lens[], int max_out) {
    fork_instance_t *inst = (fork_instance_t *)instance;
    if (!inst || max_out < 1) return 0;
    (void)frames;
    (void)sample_rate;

    if (inst->mode == 1) {
        int count = 0;
        uint8_t msg[3];
        uint8_t len;
        while (count < max_out && ring_buffer_pop(&inst->recv_ring_buf, msg, &len)) {
            if (len >= 3) {
                uint8_t status = msg[0];
                uint8_t type = status & 0xF0;
                if (type == 0x90 || type == 0x80 || type == 0xA0) {
                    int note = msg[1] + inst->transpose * 12;
                    msg[1] = (uint8_t)clamp_int(note, 0, 127);
                }
            }
            memcpy(out_msgs[count], msg, len);
            out_lens[count] = len;
            count++;
        }
        return count;
    }

    return 0;
}

static void fork_set_param(void *instance, const char *key, const char *val) {
    fork_instance_t *inst = (fork_instance_t *)instance;
    if (!inst || !key || !val) return;

    int path_changed = 0;

    if (strcmp(key, "mode") == 0) {
        if (strcmp(val, "receiver") == 0) {
            inst->mode = 1;
        } else {
            inst->mode = 0;
        }
    }
    else if (strcmp(key, "split_oct_1") == 0) {
        inst->split_oct_1 = parse_split_octave(val);
        if (inst->split_oct_1 >= 0) {
            if (inst->split_oct_2 >= 0 && inst->split_oct_2 <= inst->split_oct_1) {
                inst->split_oct_2 = -1;
            }
            if (inst->split_oct_3 >= 0 && inst->split_oct_3 <= inst->split_oct_1) {
                inst->split_oct_3 = -1;
            }
        }
    }
    else if (strcmp(key, "split_oct_2") == 0) {
        inst->split_oct_2 = parse_split_octave(val);
        if (inst->split_oct_2 >= 0) {
            if (inst->split_oct_1 >= 0 && inst->split_oct_2 <= inst->split_oct_1) {
                inst->split_oct_2 = -1;
            }
            if (inst->split_oct_3 >= 0 && inst->split_oct_3 <= inst->split_oct_2) {
                inst->split_oct_3 = -1;
            }
        }
    }
    else if (strcmp(key, "split_oct_3") == 0) {
        inst->split_oct_3 = parse_split_octave(val);
        if (inst->split_oct_3 >= 0) {
            if (inst->split_oct_2 >= 0) {
                if (inst->split_oct_3 <= inst->split_oct_2) inst->split_oct_3 = -1;
            } else if (inst->split_oct_1 >= 0) {
                if (inst->split_oct_3 <= inst->split_oct_1) inst->split_oct_3 = -1;
            }
        }
    }
    else if (strcmp(key, "transpose") == 0) {
        inst->transpose = clamp_int(atoi(val), -10, 10);
    }
    else if (strcmp(key, "split_1_chan") == 0) {
        strncpy(inst->split_1_chan_str, val, sizeof(inst->split_1_chan_str) - 1);
        inst->split_1_chan_str[sizeof(inst->split_1_chan_str) - 1] = '\0';
        int ch = atoi(val);
        if (ch >= 1 && ch <= 16) {
            inst->split1_chan = ch - 1;
        } else {
            inst->split1_chan = 0;
        }
    }
    else if (strcmp(key, "split_2_chan") == 0) {
        strncpy(inst->split_2_chan_str, val, sizeof(inst->split_2_chan_str) - 1);
        inst->split_2_chan_str[sizeof(inst->split_2_chan_str) - 1] = '\0';
        int ch = atoi(val);
        if (ch >= 1 && ch <= 16) {
            inst->split2_chan = ch - 1;
        } else {
            inst->split2_chan = 0;
        }
    }
    else if (strcmp(key, "split_3_chan") == 0) {
        strncpy(inst->split_3_chan_str, val, sizeof(inst->split_3_chan_str) - 1);
        inst->split_3_chan_str[sizeof(inst->split_3_chan_str) - 1] = '\0';
        int ch = atoi(val);
        if (ch >= 1 && ch <= 16) {
            inst->split3_chan = ch - 1;
        } else {
            inst->split3_chan = 0;
        }
    }
    else if (strcmp(key, "pipe_1_select") == 0) {
        strncpy(inst->pipe_1_select, val, sizeof(inst->pipe_1_select) - 1);
        inst->pipe_1_select[sizeof(inst->pipe_1_select) - 1] = '\0';
        path_changed = 1;
    }
    else if (strcmp(key, "pipe_2_select") == 0) {
        strncpy(inst->pipe_2_select, val, sizeof(inst->pipe_2_select) - 1);
        inst->pipe_2_select[sizeof(inst->pipe_2_select) - 1] = '\0';
        path_changed = 1;
    }
    else if (strcmp(key, "pipe_3_select") == 0) {
        strncpy(inst->pipe_3_select, val, sizeof(inst->pipe_3_select) - 1);
        inst->pipe_3_select[sizeof(inst->pipe_3_select) - 1] = '\0';
        path_changed = 1;
    }
    else if (strcmp(key, "recv_pipe_select") == 0) {
        strncpy(inst->recv_pipe_select, val, sizeof(inst->recv_pipe_select) - 1);
        inst->recv_pipe_select[sizeof(inst->recv_pipe_select) - 1] = '\0';
        path_changed = 1;
    }
    else if (strcmp(key, "state") == 0) {
        char s[64];
        int i;
        if (json_get_string(val, "mode", s, sizeof(s))) fork_set_param(inst, "mode", s);
        if (json_get_string(val, "split_oct_1", s, sizeof(s))) fork_set_param(inst, "split_oct_1", s);
        if (json_get_string(val, "split_oct_2", s, sizeof(s))) fork_set_param(inst, "split_oct_2", s);
        if (json_get_string(val, "split_oct_3", s, sizeof(s))) fork_set_param(inst, "split_oct_3", s);
        if (json_get_int(val, "transpose", &i)) { snprintf(s, sizeof(s), "%d", i); fork_set_param(inst, "transpose", s); }
        if (json_get_string(val, "split_1_chan", s, sizeof(s))) fork_set_param(inst, "split_1_chan", s);
        if (json_get_string(val, "split_2_chan", s, sizeof(s))) fork_set_param(inst, "split_2_chan", s);
        if (json_get_string(val, "split_3_chan", s, sizeof(s))) fork_set_param(inst, "split_3_chan", s);
        if (json_get_string(val, "pipe_1_select", s, sizeof(s))) fork_set_param(inst, "pipe_1_select", s);
        if (json_get_string(val, "pipe_2_select", s, sizeof(s))) fork_set_param(inst, "pipe_2_select", s);
        if (json_get_string(val, "pipe_3_select", s, sizeof(s))) fork_set_param(inst, "pipe_3_select", s);
        if (json_get_string(val, "recv_pipe_select", s, sizeof(s))) fork_set_param(inst, "recv_pipe_select", s);
    }

    if (path_changed) {
        pthread_mutex_lock(&inst->path_mutex);
        resolve_pipe_path(inst->pipe_1_select, "midifork1", inst->next_split1_path, sizeof(inst->next_split1_path));
        resolve_pipe_path(inst->pipe_2_select, "midifork2", inst->next_split2_path, sizeof(inst->next_split2_path));
        resolve_pipe_path(inst->pipe_3_select, "midifork3", inst->next_split3_path, sizeof(inst->next_split3_path));
        resolve_pipe_path(inst->recv_pipe_select, "midifork1", inst->next_recv_path, sizeof(inst->next_recv_path));
        inst->paths_dirty = 1;
        pthread_mutex_unlock(&inst->path_mutex);
    }
}

static void format_octave_options(char *buf, size_t max_len, int min_index) {
    size_t len = snprintf(buf, max_len, "[\"off\"");
    for (int i = min_index; i <= 9; i++) {
        const char *name = format_split_octave(i);
        if (len < max_len) {
            len += snprintf(buf + len, max_len - len, ",\"%s\"", name);
        }
    }
    if (len < max_len) {
        snprintf(buf + len, max_len - len, "]");
    }
}

static int generate_dynamic_chain_params(fork_instance_t *inst, char *buf, int buf_len) {
    char opt1[128], opt2[128], opt3[128];
    format_octave_options(opt1, sizeof(opt1), 0);
    
    int min2 = (inst->split_oct_1 >= 0) ? (inst->split_oct_1 + 1) : 0;
    format_octave_options(opt2, sizeof(opt2), min2);
    
    int min3 = 0;
    if (inst->split_oct_2 >= 0) {
        min3 = inst->split_oct_2 + 1;
    } else if (inst->split_oct_1 >= 0) {
        min3 = inst->split_oct_1 + 1;
    }
    format_octave_options(opt3, sizeof(opt3), min3);
    
    return snprintf(buf, (size_t)buf_len,
        "["
        "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"splitter\",\"receiver\"]},"
        "{\"key\":\"split_oct_1\",\"name\":\"Split 1 Oct\",\"type\":\"enum\",\"options\":%s},"
        "{\"key\":\"split_oct_2\",\"name\":\"Split 2 Oct\",\"type\":\"enum\",\"options\":%s},"
        "{\"key\":\"split_oct_3\",\"name\":\"Split 3 Oct\",\"type\":\"enum\",\"options\":%s},"
        "{\"key\":\"transpose\",\"name\":\"Transpose\",\"type\":\"int\",\"min\":-10,\"max\":10,\"step\":1},"
        "{\"key\":\"split_1_chan\",\"name\":\"Split 1 Chan\",\"type\":\"enum\",\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\"]},"
        "{\"key\":\"split_2_chan\",\"name\":\"Split 2 Chan\",\"type\":\"enum\",\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\"]},"
        "{\"key\":\"split_3_chan\",\"name\":\"Split 3 Chan\",\"type\":\"enum\",\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\"]},"
        "{\"key\":\"pipe_1_select\",\"name\":\"Pipe 1\",\"type\":\"enum\",\"options\":[\"off\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]},"
        "{\"key\":\"pipe_2_select\",\"name\":\"Pipe 2\",\"type\":\"enum\",\"options\":[\"off\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]},"
        "{\"key\":\"pipe_3_select\",\"name\":\"Pipe 3\",\"type\":\"enum\",\"options\":[\"off\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]},"
        "{\"key\":\"recv_pipe_select\",\"name\":\"Recv Pipe\",\"type\":\"enum\",\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]}"
        "]",
        opt1, opt2, opt3
    );
}

static int fork_get_param(void *instance, const char *key, char *buf, int buf_len) {
    fork_instance_t *inst = (fork_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", inst->mode ? "receiver" : "splitter");
    }
    if (strcmp(key, "split_oct_1") == 0) {
        return snprintf(buf, buf_len, "%s", format_split_octave(inst->split_oct_1));
    }
    if (strcmp(key, "split_oct_2") == 0) {
        return snprintf(buf, buf_len, "%s", format_split_octave(inst->split_oct_2));
    }
    if (strcmp(key, "split_oct_3") == 0) {
        return snprintf(buf, buf_len, "%s", format_split_octave(inst->split_oct_3));
    }
    if (strcmp(key, "transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->transpose);
    }
    if (strcmp(key, "split_1_chan") == 0) {
        return snprintf(buf, buf_len, "%s", inst->split_1_chan_str);
    }
    if (strcmp(key, "split_2_chan") == 0) {
        return snprintf(buf, buf_len, "%s", inst->split_2_chan_str);
    }
    if (strcmp(key, "split_3_chan") == 0) {
        return snprintf(buf, buf_len, "%s", inst->split_3_chan_str);
    }
    if (strcmp(key, "pipe_1_select") == 0) {
        return snprintf(buf, buf_len, "%s", inst->pipe_1_select);
    }
    if (strcmp(key, "pipe_2_select") == 0) {
        return snprintf(buf, buf_len, "%s", inst->pipe_2_select);
    }
    if (strcmp(key, "pipe_3_select") == 0) {
        return snprintf(buf, buf_len, "%s", inst->pipe_3_select);
    }
    if (strcmp(key, "recv_pipe_select") == 0) {
        return snprintf(buf, buf_len, "%s", inst->recv_pipe_select);
    }
    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Fork");
    }
    if (strcmp(key, "bank_name") == 0) {
        return snprintf(buf, buf_len, "Factory");
    }
    if (strcmp(key, "chain_params") == 0) {
        return generate_dynamic_chain_params(inst, buf, buf_len);
    }
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"mode\":\"%s\",\"split_oct_1\":\"%s\",\"split_oct_2\":\"%s\",\"split_oct_3\":\"%s\","
            "\"transpose\":%d,\"split_1_chan\":\"%s\",\"split_2_chan\":\"%s\",\"split_3_chan\":\"%s\","
            "\"pipe_1_select\":\"%s\",\"pipe_2_select\":\"%s\",\"pipe_3_select\":\"%s\","
            "\"recv_pipe_select\":\"%s\"}",
            inst->mode ? "receiver" : "splitter",
            format_split_octave(inst->split_oct_1),
            format_split_octave(inst->split_oct_2),
            format_split_octave(inst->split_oct_3),
            inst->transpose,
            inst->split_1_chan_str,
            inst->split_2_chan_str,
            inst->split_3_chan_str,
            inst->pipe_1_select,
            inst->pipe_2_select,
            inst->pipe_3_select,
            inst->recv_pipe_select
        );
    }
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = fork_create_instance,
    .destroy_instance = fork_destroy_instance,
    .process_midi = fork_process_midi,
    .tick = fork_tick,
    .set_param = fork_set_param,
    .get_param = fork_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
