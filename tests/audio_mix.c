/* Exercise the actual shared mixer without a device, thread or guest assets. */
#include "../external/ps3recomp/libs/audio/cellAudio.c"
uint8_t* vm_base;

static void sample(float* where, float value)
{
    uint32_t bits; memcpy(&bits, &value, 4);
    uint8_t* bytes = (uint8_t*)where;
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(bits >> (24 - i * 8));
}
int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "device_missing")) {
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        return 1;
    }
    if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
    mutex_init(&s_audio_mutex); s_audio_initialized = 1;
    for (unsigned channels = 2; channels <= 8; channels += 6) {
        for (unsigned blocks = 8; blocks <= 32; blocks *= 2) {
            vm_write64(0x30000, channels); vm_write64(0x30008, blocks);
            vm_write64(0x30010, 0);
            if (cellAudioPortOpen((void*)0x30000, (void*)0x30100)) return 2;
            AudioPortSlot* port = &s_ports[0];
            for (unsigned ch = 0; ch < channels; ++ch) sample(port->buffer + ch * channels + ch, 0.25f);
            audio_mix_one_block();
            if (port->read_index || ld_be_f32(port->buffer) != 0.25f) return 3;
            if (cellAudioPortStart(0) || cellAudioPortStart(0) != CELL_AUDIO_ERROR_PORT_ALREADY_RUN) return 4;
            if (ld_be_f32(port->buffer) != 0) return 13;
            for (unsigned ch = 0; ch < channels; ++ch) sample(port->buffer + ch * channels + ch, 0.25f);
            audio_mix_one_block();
            for (unsigned ch = 0; ch < channels; ++ch) {
                float expected[2] = {0, 0};
                if (channels == 2) expected[ch] = 0.25f;
                else if (ch < 2) expected[ch] = 0.17675f;
                else if (ch == 2) expected[0] = expected[1] = 0.125f;
                else if (ch > 3) expected[ch & 1] = 0.125f;
                for (unsigned side = 0; side < 2; ++side)
                    if (fabsf(s_mix_buffer[ch * 2 + side] - expected[side]) > 0.000001f) return 5;
            }
            for (unsigned i = 0; i < 256 * channels; ++i) if (port->buffer[i] != 0) return 6;
            if (port->read_index != 1 || vm_read64((u32)port->read_idx_addr) != 1) return 7;
            if (cellAudioPortStop(0) || cellAudioPortStop(0) != CELL_AUDIO_ERROR_PORT_NOT_RUN) return 8;
            audio_mix_one_block();
            if (port->read_index != 1) return 9;
            if (cellAudioPortStart(0)) return 10;
            for (unsigned i = 1; i < blocks * 3; ++i) audio_mix_one_block();
            if (port->read_index != blocks * 3 || vm_read64((u32)port->read_idx_addr)) return 11;
            port->param.level = 0;
            sample(port->buffer, 0.25f); audio_mix_one_block();
            for (unsigned i = 0; i < 512; ++i) if (s_mix_buffer[i] != 0) return 14;
            port->param.level = 1;
            sample(port->buffer + 256 * channels, 2.0f);
            sample(port->buffer + 256 * channels + 1, -2.0f);
            audio_mix_one_block();
            if (s_mix_buffer[0] != 1 || s_mix_buffer[1] != -1) return 15;
            if (cellAudioPortClose(0) || cellAudioPortStart(0) != CELL_AUDIO_ERROR_PORT_NOT_OPEN ||
                cellAudioPortStop(0) != CELL_AUDIO_ERROR_PORT_NOT_RUN ||
                cellAudioPortStart(8) != CELL_AUDIO_ERROR_PARAM || cellAudioPortStop(8) != CELL_AUDIO_ERROR_PARAM ||
                cellAudioPortClose(8) != CELL_AUDIO_ERROR_PARAM) return 12;
        }
    }
    s_audio_initialized = 0; mutex_destroy(&s_audio_mutex); vm_shutdown();
    puts("Verified BE PCM lanes, stereo matrix, LFE exclusion, block clearing, wraps and port lifecycle");
    return 0;
}
