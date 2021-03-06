/*
 * Copyright (C) 2016, The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_amplifier::tfa"
//#define LOG_NDEBUG 0

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <cutils/log.h>
#include <tinyalsa/asoundlib.h>
#include "tfa.h"
#include "tfa9888.h"
#include "tfa9888-debug.h"
#include "tfa9888-patches.h"

struct tfaS {
    struct mixer *mixer;
    int fd;
};

#define SND_CARD        0
#define AMP_PCM_DEV     47
#define AMP_MIXER_CTL   "QUAT_MI2S_RX_DL_HL Switch"

tfa_t *tfa_new(void)
{
    struct mixer *mixer;
    int fd;
    tfa_t *t;

    if ((mixer = mixer_open(SND_CARD)) == NULL) {
        ALOGE("failed to open mixer");
        return NULL;
    }

    if ((fd = open("/dev/tfa9888", O_RDWR)) < 0) {
        ALOGE("failed to open /dev/tfa9888");
        mixer_close(mixer);
        return NULL;
    }

    if ((t = malloc(sizeof(*t))) == NULL) {
        ALOGE("out of memory");
        mixer_close(mixer);
        return NULL;
    }

    t->mixer = mixer;
    t->fd = fd;

    return t;
}

void tfa_destroy(tfa_t *t)
{
    mixer_close(0);
    close(t->fd);
    free(t);
}

static struct pcm_config amp_pcm_config = {
    .channels = 1,
    .rate = 48000,
    .period_size = 0,
    .period_count = 4,
    .format = 0,
    .start_threshold = 0,
    .stop_threshold = INT_MAX,
    .avail_min = 0,
};

struct pcm *tfa_mi2s_enable(tfa_t *t)
{
    struct mixer_ctl *ctl;
    struct pcm *pcm;
    struct pcm_params *pcm_params;

    ctl = mixer_get_ctl_by_name(t->mixer, AMP_MIXER_CTL);
    if (ctl == NULL) {
        ALOGE("%s: Could not find %s\n", __func__, AMP_MIXER_CTL);
        return NULL;
    }

    pcm_params = pcm_params_get(SND_CARD, AMP_PCM_DEV, PCM_OUT);
    if (pcm_params == NULL) {
        ALOGE("Could not get the pcm_params\n");
        return NULL;
    }

    amp_pcm_config.period_count = pcm_params_get_max(pcm_params, PCM_PARAM_PERIODS);
    pcm_params_free(pcm_params);

    mixer_ctl_set_value(ctl, 0, 1);
    pcm = pcm_open(SND_CARD, AMP_PCM_DEV, PCM_OUT, &amp_pcm_config);
    if (!pcm) {
        ALOGE("failed to open pcm at all??");
        return NULL;
    }
    if (!pcm_is_ready(pcm)) {
        ALOGE("failed to open pcm device: %s", pcm_get_error(pcm));
        pcm_close(pcm);
        return NULL;
    }

    return pcm;
}

int tfa_mi2s_disable(tfa_t *t, struct pcm *pcm)
{
    struct mixer_ctl *ctl;

    pcm_close(pcm);

    ctl = mixer_get_ctl_by_name(t->mixer, AMP_MIXER_CTL);
    if (ctl == NULL) {
        ALOGE("%s: Could not find %s\n", __func__, AMP_MIXER_CTL);
        return -ENODEV;
    } else {
        mixer_ctl_set_value(ctl, 0, 0);
    }

    return 0;
}

struct pcm *tfa_power_on(tfa_t *t)
{
    struct pcm *pcm;
    int retry;

    pcm = tfa_mi2s_enable(t);
    tfa_set_bitfield(t, TFA98XX_BF_POWERDOWN, 0);
    for (retry = 0; retry < 1000; retry++) {
        if (!tfa_get_bitfield(t, TFA98XX_BF_FLAG_LOST_CLK)) {
            return pcm;
        }
        usleep(1000);
    }
    ALOGE("Failed to power on the amps (no clocks)\n");
    tfa_mi2s_disable(t, pcm);
    return NULL;
}

void tfa_power_off(tfa_t *t, struct pcm *pcm)
{
    tfa_set_bitfield(t, TFA98XX_BF_POWERDOWN, 1);
    tfa_mi2s_disable(t, pcm);
}

static inline unsigned bf_mask(int bf)
{
    return (1<<((bf&0xf)+1)) - 1;
}

static inline unsigned bf_shift(int bf)
{
    return (bf & 0xf0) >> 4;
}

static inline unsigned bf_register(int bf)
{
    return bf >> 8;
}

int tfa_set_bitfield(tfa_t *t, int bf, unsigned value)
{
    unsigned char cmd[4];
    unsigned old;
    unsigned shifted_mask = bf_mask(bf) << bf_shift(bf);

    old = tfa_get_register(t, bf_register(bf));
    value &= bf_mask(bf);

    return tfa_set_register(t, bf_register(bf), (old & ~shifted_mask) | (value << bf_shift(bf)));
}

int tfa_get_bitfield(tfa_t *t, int bf)
{
    int v = tfa_get_register(t, bf_register(bf));
    if (v < 0) return v;
    return (v >> bf_shift(bf)) & bf_mask(bf);
}

int tfa_set_register(tfa_t *t, unsigned reg, unsigned value)
{
    unsigned char cmd[3];
    unsigned old;

    cmd[0] = reg;
    cmd[1] = value >> 8;
    cmd[2] = value;

    printf("SET %x: ", reg);
    tfa9888_print_bitfield(stdout, reg, value);

    return write(t->fd, cmd, 3);
}

int tfa_get_register(tfa_t *t, unsigned reg)
{
    unsigned char buf[2];
    int res;
    int v;

    buf[0] = reg;
    if ((res = write(t->fd, buf, 1)) < 0) return res;
    if ((res = read(t->fd, buf, 2)) < 0) return res;
    v = (buf[0]<<8) | buf[1];

    printf("GET %x: ", reg);
    tfa9888_print_bitfield(stdout, reg, v);

    return v;
}

static void setup_keys(tfa_t *t)
{
    unsigned short mtpdataB;

    tfa_set_register(t, 0xf, 23147);
    mtpdataB = tfa_get_bitfield(t, TFA98XX_BF_MTPDATAB);
    tfa_set_register(t, 0xa0, mtpdataB ^ 0x5a);
}

static void setup_basic_registers(tfa_t *t)
{
    tfa_set_register(t, 0x00, 0x164d);
    tfa_set_register(t, 0x01, 0x828b);
    tfa_set_register(t, 0x02, 0x1dc8);
    tfa_set_register(t, 0x0e, 0x80);
    tfa_set_register(t, 0x20, 0x89e);
    tfa_set_register(t, 0x22, 0x543c);
    tfa_set_register(t, 0x23, 0x06);
    tfa_set_register(t, 0x24, 0x14);
    tfa_set_register(t, 0x25, 0x0a);
    tfa_set_register(t, 0x26, 0x100);
    tfa_set_register(t, 0x28, 0x1000);
    tfa_set_register(t, 0x51, 0x00);
    tfa_set_register(t, 0x52, 0xfafe);
    tfa_set_register(t, 0x70, 0x3ee4);
    tfa_set_register(t, 0x71, 0x1074);
    tfa_set_register(t, 0x83, 0x14);
}

static void setup_tdm(tfa_t *t)
{
    tfa_set_bitfield(t, TFA98XX_BF_TDM_SAMPLE_SIZE, 15);
    tfa_set_bitfield(t, TFA98XX_BF_TDM_NBCK, 2);
    tfa_set_bitfield(t, TFA98XX_BF_TDM_SOURCE6_IO, 0);
    tfa_set_bitfield(t, TFA98XX_BF_TDM_SOURCE5_IO, 0);
    tfa_set_bitfield(t, TFA98XX_BF_TDM_SOURCE5_SLOT, 1);
    tfa_set_bitfield(t, TFA98XX_BF_TDM_SOURCE7_SLOT, 1);
    tfa_set_bitfield(t, TFA98XX_BF_TDM_SOURCE8_SLOT, 0);
}

static void send_patches(tfa_t *t, tfa_patch_t *p, size_t n)
{
    size_t i;

printf("sending %d patches\n", (int) n);
    for (i = 0; i < n; i++) {
        ssize_t n = write(t->fd, p[i].data, p[i].len);
size_t j;
printf("WR");
for (j = 0; j < p[i].len; j++) printf(" %02x", p[i].data[j]);
printf("\n");
        if (n <= 0) {
            ALOGW("Failed to send patch #%d\n", (int) i+1);
        }
    }
}

int tfa_startup(tfa_t *t)
{
    struct pcm *pcm;

    tfa_set_register(t, 0, 0x02);               // RESET
    tfa_set_bitfield(t, TFA98XX_BF_CF_RST_DSP, 1);

    setup_keys(t);
    setup_basic_registers(t);
    setup_tdm(t);

    tfa_set_bitfield(t, TFA98XX_BF_ENBL_POWERSWITCH, 1);
    tfa_set_bitfield(t, TFA98XX_BF_ENBL_PDM_SS, 0);

    tfa_set_bitfield(t, TFA98XX_BF_ENBL_BOOST, 1);
    tfa_set_bitfield(t, TFA98XX_BF_BOOST_VOLT, 6);
    tfa_set_bitfield(t, TFA98XX_BF_ENBL_BOD, 1);
    tfa_set_bitfield(t, TFA98XX_BF_EXT_TEMP, 25);
    tfa_set_bitfield(t, TFA98XX_BF_EXT_TEMP_SEL, 1);
    tfa_set_bitfield(t, TFA98XX_BF_ENBL_BOOST, 0);

    pcm = tfa_power_on(t);

    tfa_set_bitfield(t, TFA98XX_BF_CF_RST_DSP, 1);
    send_patches(t, tfa98xx_patches_1, n_tfa98xx_patches_1);
    tfa_set_bitfield(t, TFA98XX_BF_CF_DMEM, 2);
    send_patches(t, tfa98xx_patches_2, n_tfa98xx_patches_2);
    tfa_set_bitfield(t, TFA98XX_BF_CF_DMEM, 3);
    send_patches(t, tfa98xx_patches_2, n_tfa98xx_patches_3);
    tfa_set_bitfield(t, TFA98XX_BF_CF_DMEM, 1);
    send_patches(t, tfa98xx_patches_2, n_tfa98xx_patches_4);
    tfa_set_register(t, 0x91, 0x200);

    tfa_power_off(t, pcm);

    return 0;
}
