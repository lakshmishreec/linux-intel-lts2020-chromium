/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2020 Intel Corporation.
 */

/*
 * This file defines data structures used in Machine Driver for Intel
 * platforms with Maxim Codecs.
 */
#ifndef __SOF_MAXIM_COMMON_H
#define __SOF_MAXIM_COMMON_H

#include <sound/soc.h>

#define MAX_98373_CODEC_DAI	"max98373-aif1"
#define MAX_98373_DEV0_NAME	"i2c-MX98373:00"
#define MAX_98373_DEV1_NAME	"i2c-MX98373:01"

extern struct snd_soc_dai_link_component max_98373_components[2];
extern struct snd_soc_ops max_98373_ops;
extern const struct snd_soc_dapm_route max_98373_dapm_routes[];

int max_98373_spk_codec_init(struct snd_soc_pcm_runtime *rtd);
void max_98373_set_codec_conf(struct snd_soc_card *card);
int max_98373_trigger(struct snd_pcm_substream *substream, int cmd);

/*
 * Maxim MAX98390
 */
#define MAX_98390_CODEC_DAI	"max98390-aif1"
#define MAX_98390_DEV0_NAME	"i2c-MX98390:00"
#define MAX_98390_DEV1_NAME	"i2c-MX98390:01"

extern struct snd_soc_dai_link_component max_98390_components[2];
extern const struct snd_soc_ops max_98390_ops;

void max_98390_set_codec_conf(struct snd_soc_card *card);

/*
 * Maxim MAX98357A/MAX98360A
 */
#define MAX_98357A_CODEC_DAI	"HiFi"
#define MAX_98357A_DEV0_NAME	"MX98357A:00"
#define MAX_98360A_DEV0_NAME	"MX98360A:00"

void max_98357a_dai_link(struct snd_soc_dai_link *link);
void max_98360a_dai_link(struct snd_soc_dai_link *link);

#endif /* __SOF_MAXIM_COMMON_H */
