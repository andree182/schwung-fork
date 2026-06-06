#define _XOPEN_SOURCE 500
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

// Include fork.c directly to access its internal structure for white-box testing
#include "../src/dsp/fork.c"

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    fork_instance_t *fork_inst;

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->set_param || !api->get_param || !api->destroy_instance) {
        fail("Fork API init failed");
    }

    inst = api->create_instance(".", NULL);
    if (!inst) fail("create_instance returned NULL");
    fork_inst = (fork_instance_t *)inst;

    // 1. Verify defaults
    assert(fork_inst->mode == 0);
    assert(fork_inst->split_oct_1 == -1);
    assert(fork_inst->split_oct_2 == -1);
    assert(fork_inst->split_oct_3 == -1);
    assert(fork_inst->transpose == 0);
    assert(fork_inst->split1_chan == 0); // Absolute index 0
    assert(fork_inst->split2_chan == 1); // Absolute index 1
    assert(fork_inst->split3_chan == 2); // Absolute index 2
    assert(strcmp(fork_inst->pipe_1_select, "1") == 0);
    assert(strcmp(fork_inst->pipe_2_select, "2") == 0);
    assert(strcmp(fork_inst->pipe_3_select, "3") == 0);
    assert(strcmp(fork_inst->recv_pipe_select, "1") == 0);

    // 2. Verify Parameter Roundtrips
    api->set_param(inst, "mode", "receiver");
    assert(fork_inst->mode == 1);
    api->set_param(inst, "mode", "splitter");
    assert(fork_inst->mode == 0);

    api->set_param(inst, "split_oct_1", "C3"); // Octave 4, Note 48
    assert(fork_inst->split_oct_1 == 4);
    api->set_param(inst, "split_oct_2", "C4"); // Octave 5, Note 60
    assert(fork_inst->split_oct_2 == 5);

    api->set_param(inst, "split_oct_1", "off");
    assert(fork_inst->split_oct_1 == -1);
    api->set_param(inst, "split_oct_1", "C3");

    api->set_param(inst, "transpose", "-2");
    assert(fork_inst->transpose == -2);
    char buf[64];
    api->get_param(inst, "transpose", buf, sizeof(buf));
    assert(strcmp(buf, "-2") == 0);

    api->set_param(inst, "transpose", "-10");
    assert(fork_inst->transpose == -10);
    api->get_param(inst, "transpose", buf, sizeof(buf));
    assert(strcmp(buf, "-10") == 0);

    api->set_param(inst, "transpose", "10");
    assert(fork_inst->transpose == 10);
    api->get_param(inst, "transpose", buf, sizeof(buf));
    assert(strcmp(buf, "10") == 0);

    api->set_param(inst, "transpose", "-2");

    api->set_param(inst, "split_1_chan", "10"); // Absolute channel 10 -> index 9
    assert(fork_inst->split1_chan == 9);

    api->set_param(inst, "split_2_chan", "5"); // Absolute channel 5 -> index 4
    assert(fork_inst->split2_chan == 4);

    api->set_param(inst, "split_3_chan", "16"); // Absolute channel 16 -> index 15
    assert(fork_inst->split3_chan == 15);

    api->set_param(inst, "pipe_1_select", "3");
    assert(strcmp(fork_inst->pipe_1_select, "3") == 0);

    api->set_param(inst, "pipe_3_select", "4");
    assert(strcmp(fork_inst->pipe_3_select, "4") == 0);

    api->set_param(inst, "split_oct_3", "C5"); // Octave 6, Note 72
    assert(fork_inst->split_oct_3 == 6);

    // 3. Test Routing in Splitter Mode
    // split_oct_1 = C3 (48), split_oct_2 = C4 (60)
    // split_1_chan = absolute 10 (index 9)
    // split_2_chan = absolute 5 (index 4)
    // transpose = -2 (transposes down 24 semitones for main)
    
    uint8_t out_msgs[16][3];
    int out_lens[16];
    int count;

    // A. Main Note: Note 40 (E2 < 48)
    uint8_t note_main[3] = { 0x90, 40, 100 };
    count = api->process_midi(inst, note_main, 3, out_msgs, out_lens, 16);
    assert(count == 1);
    assert(out_lens[0] == 3);
    assert(out_msgs[0][0] == 0x90);
    assert(out_msgs[0][1] == 16); // 40 - 24 = 16
    assert(out_msgs[0][2] == 100);

    // B. Split 1 Note: Note 50 (D3 >= 48 and < 60)
    uint8_t note_split1[3] = { 0x90, 50, 100 };
    count = api->process_midi(inst, note_split1, 3, out_msgs, out_lens, 16);
    assert(count == 0); // Intercepted, sent to pipe

    // Inspect Split 1 buffer (no transposition on split notes)
    uint8_t pop_msg[3];
    uint8_t pop_len;
    int pop_ok = ring_buffer_pop(&fork_inst->split1_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_len == 3);
    assert(pop_msg[0] == 0x99); // Channel changed to 9 (Absolute 10)
    assert(pop_msg[1] == 50);   // Untransposed
    assert(pop_msg[2] == 100);

    // C. Split 2 Note: Note 64 (E4 >= 60 and < 72)
    uint8_t note_split2[3] = { 0x91, 64, 90 };
    count = api->process_midi(inst, note_split2, 3, out_msgs, out_lens, 16);
    assert(count == 0); // Intercepted

    // Inspect Split 2 buffer
    pop_ok = ring_buffer_pop(&fork_inst->split2_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_len == 3);
    assert(pop_msg[0] == 0x94); // Absolute channel 5 -> index 4
    assert(pop_msg[1] == 64);   // Untransposed
    assert(pop_msg[2] == 90);

    // D. Split 3 Note: Note 75 (D5 >= 72)
    uint8_t note_split3[3] = { 0x90, 75, 100 };
    count = api->process_midi(inst, note_split3, 3, out_msgs, out_lens, 16);
    assert(count == 0); // Intercepted

    // Inspect Split 3 buffer
    pop_ok = ring_buffer_pop(&fork_inst->split3_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_len == 3);
    assert(pop_msg[0] == 0x9F); // Absolute channel 16 -> index 15
    assert(pop_msg[1] == 75);   // Untransposed
    assert(pop_msg[2] == 100);

    // 4. Test Active Note Destination Tracking (avoid stuck notes)
    uint8_t note_on_50[3] = { 0x90, 50, 100 };
    api->process_midi(inst, note_on_50, 3, out_msgs, out_lens, 16);
    
    api->set_param(inst, "split_oct_1", "off");
    
    uint8_t note_off_50[3] = { 0x80, 50, 0 };
    count = api->process_midi(inst, note_off_50, 3, out_msgs, out_lens, 16);
    assert(count == 0); // Should still be intercepted since the Note On went to Split 1!
    
    // Pop Note On 50 from split1 buffer
    pop_ok = ring_buffer_pop(&fork_inst->split1_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_msg[0] == 0x99);
    assert(pop_msg[1] == 50);
    
    // Pop Note Off 50 from split1 buffer
    pop_ok = ring_buffer_pop(&fork_inst->split1_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_msg[0] == 0x89);
    assert(pop_msg[1] == 50);
    
    // 5. Test Global Message Broadcasting
    api->set_param(inst, "split_oct_1", "C3"); // Reactivate Split 1
    uint8_t cc_msg[3] = { 0xB0, 74, 120 };
    count = api->process_midi(inst, cc_msg, 3, out_msgs, out_lens, 16);
    assert(count == 1); // Main gets CC

    pop_ok = ring_buffer_pop(&fork_inst->split1_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_msg[0] == 0xB9); // Channel adjusted to index 9

    pop_ok = ring_buffer_pop(&fork_inst->split2_ring_buf, pop_msg, &pop_len);
    assert(pop_ok == 1);
    assert(pop_msg[0] == 0xB4); // Channel adjusted to index 4

    // 5b. Test Global Message Broadcasting with pipe select "off"
    api->set_param(inst, "pipe_1_select", "off");
    api->set_param(inst, "pipe_2_select", "off");
    api->set_param(inst, "pipe_3_select", "off");
    count = api->process_midi(inst, cc_msg, 3, out_msgs, out_lens, 16);
    assert(count == 4); // Main CC + Split 1 CC + Split 2 CC + Split 3 CC
    
    assert(out_msgs[0][0] == 0xB0); // Original
    assert(out_msgs[0][1] == 74);
    
    assert(out_msgs[1][0] == 0xB9); // Split 1 re-channeled to absolute 10
    assert(out_msgs[1][1] == 74);
    
    assert(out_msgs[2][0] == 0xB4); // Split 2 re-channeled to absolute 5
    assert(out_msgs[2][1] == 74);
    
    assert(out_msgs[3][0] == 0xBF); // Split 3 re-channeled to absolute 16 (index 15)
    assert(out_msgs[3][1] == 74);

    // Note splitting directly on track
    // Split 1 Note: Note 50 (D3 >= 48 and < 60)
    count = api->process_midi(inst, note_split1, 3, out_msgs, out_lens, 16);
    assert(count == 1); // Returned directly on track
    assert(out_msgs[0][0] == 0x99); // Channel adjusted to absolute 10 (index 9)
    assert(out_msgs[0][1] == 50);

    // 6. Test Receiver Mode & Transposition
    api->set_param(inst, "mode", "receiver");
    assert(fork_inst->mode == 1);

    api->set_param(inst, "transpose", "1"); // Transpose +12 semitones
    assert(fork_inst->transpose == 1);

    uint8_t piped_msg[3] = { 0x91, 60, 100 }; // Note On on Channel 2, Note 60
    ring_buffer_push(&fork_inst->recv_ring_buf, piped_msg, 3);
    count = api->tick(inst, 128, 44100, out_msgs, out_lens, 16);
    assert(count == 1);
    assert(out_msgs[0][0] == 0x91); // Kept Channel 2 as-is
    assert(out_msgs[0][1] == 72);   // 60 + 12 = 72

    api->destroy_instance(inst);
    printf("PASS: Fork unit tests\n");
    return 0;
}
