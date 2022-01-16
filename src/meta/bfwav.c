#include "meta.h"
#include "../coding/coding.h"
#include "../util/endianness.h"


/* FWAV and CWAV are basically identical except always LE */
typedef enum { FWAV, CWAV } bxwav_type_t;

static VGMSTREAM* init_vgmstream_bxwav(STREAMFILE* sf, bxwav_type_t type);

/* FWAV - NintendoWare binary caFe wave (WiiU and Switch games) */
VGMSTREAM* init_vgmstream_bfwav(STREAMFILE* sf) {

    /* checks */
    if (!is_id32be(0x00, sf, "FWAV"))
        goto fail;

    /* .bfwav: used?
     * .fwav: header id */
    if (!check_extensions(sf, "bfwav,fwav"))
        goto fail;

    return init_vgmstream_bxwav(sf, FWAV);

fail:
    return NULL;
}

/* CWAV - NintendoWare binary CTR wave (3DS games) */
VGMSTREAM* init_vgmstream_bcwav(STREAMFILE* sf) {

    /* checks */
    if (!is_id32be(0x00, sf, "CWAV"))
        goto fail;

    /* .bcwav: standard 3DS (though rare as usually found in .bcsar) [Adventure Bar Story (3DS), LBX (3DS)]
     * .adpcm: 80's Overdrive (3DS)
     * .bms: 3D Classics Kirby's Adventure (3DS)
     * .sfx: Wizdom (3DS)
     * .str: Pac-Man and the Ghostly Adventures 2 (3DS)
     * .zic: Wizdom (3DS) */
    if (!check_extensions(sf, "bcwav,adpcm,bms,sfx,str,zic"))
        goto fail;

    return init_vgmstream_bxwav(sf, CWAV);

fail:
    return NULL;
}


static VGMSTREAM* init_vgmstream_bxwav(STREAMFILE* sf, bxwav_type_t type) {
    VGMSTREAM* vgmstream = NULL;

    uint32_t info_offset, data_offset, chtb_offset;
    int channels, loop_flag, codec, sample_rate;
    int big_endian;
    int32_t num_samples, loop_start;

    read_u32_t read_u32;
    read_s32_t read_s32;
    read_u16_t read_u16;
    read_s16_t read_s16;

    /* BOM check */
    if (read_u16be(0x04, sf) == 0xFEFF) { /* WiiU */
        big_endian = 1;
        read_u32 = read_u32be;
        read_s32 = read_s32be;
        read_u16 = read_u16be;
        read_s16 = read_s16be;
    }
    else if (read_u16le(0x04, sf) == 0xFEFF) { /* 3DS, Switch */
        big_endian = 0;
        read_u32 = read_u32le;
        read_s32 = read_s32le;
        read_u16 = read_u16le;
        read_s16 = read_s16le;
    }
    else {
        goto fail;
    }

    /* header */
    /* 0x06(2): header size (0x40) */
    /* 0x08: version */
    /* - FWAV: 0x00010200 */
    /* - CWAV: 0x00000002 (Kirby's Adventure), 0x00000102 (common), 0x00010102 (FE Fates, Hyrule Warriors Legends) */
    /* 0x0c: file size */
    /* 0x10(2): sections (2) */

    /* 0x14(2): info mark (0x7000) */
    info_offset = read_u32(0x18, sf);
    /* 0x1c: info size */

    /* 0x20(2): data mark (0x7001) */
    data_offset = read_u32(0x24, sf);
    /* 0x28: data size */
    /* rest: padding */


    /* INFO section */
    if (!is_id32be(info_offset + 0x00, sf, "INFO"))
        goto fail;
    /* 0x04: size */
    codec = read_u8(info_offset + 0x08, sf);
    loop_flag = read_u8(info_offset + 0x09, sf);
    /* 0x0a: padding */
    sample_rate = read_u32(info_offset + 0x0C, sf);
    loop_start  = read_s32(info_offset + 0x10, sf);
    num_samples = read_s32(info_offset + 0x14, sf);
    /* 0x18: original loop start? (slightly lower) */
    chtb_offset = info_offset + 0x1C;
    channels    = read_u32(chtb_offset + 0x00, sf);
    /* channel table is parsed at the end */

    /* DATA section */
    if (!is_id32be(data_offset + 0x00, sf, "DATA"))
        goto fail;


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channels, loop_flag);
    if (!vgmstream) goto fail;

    switch(type) {
        case FWAV: vgmstream->meta_type = meta_FWAV; break;
        case CWAV: vgmstream->meta_type = meta_CWAV; break;
        default: goto fail;
    }
    vgmstream->sample_rate = sample_rate;

    vgmstream->num_samples = num_samples;
    vgmstream->loop_start_sample = loop_start;
    vgmstream->loop_end_sample = num_samples;
    if (type == CWAV)
        vgmstream->allow_dual_stereo = 1; /* LEGO 3DS games */
    
    vgmstream->layout_type = layout_none;

    /* only 0x02 is known, other codecs are probably from bxstm that do use them */
    switch (codec) {
        case 0x00:
            vgmstream->coding_type = coding_PCM8;
            break;

        case 0x01:
            vgmstream->coding_type = big_endian ? coding_PCM16BE : coding_PCM16LE;
            break;

        case 0x02: /* common */
            vgmstream->coding_type = coding_NGC_DSP;
            /* coefs are read below */
            break;

        case 0x03:
            vgmstream->coding_type = coding_3DS_IMA;
            /* hist is read below */
            break;

        default:
            goto fail;
    }


    if (!vgmstream_open_stream_bf(vgmstream, sf, data_offset, 1))
        goto fail;

    /* parse channel table and offsets
     * (usually the interleave/distance is fixed, but in theory could be non-standard, so assign manually) */
    {
        int ch, i;
        for (ch = 0; ch < channels; ch++) {
            uint32_t chnf_offset, chdt_offset;
            /* channel entry: */
            /* - 0x00: mark (0x7100) */
            /* - 0x02: padding */
            /* - 0x04: channel info offset (from channel table offset) */
            chnf_offset = read_u32(chtb_offset + 0x04 + ch * 0x08 + 0x04, sf) + chtb_offset;

            /* channel info: */
            /* 0x00: mark (0x1F00) */
            /* 0x02: padding */
            /* 0x04: offset to channel data (from DATA offset after size ) */
            /* 0x08: ADPCM mark (0x0300=DSP, 0x0301=IMA, 0x0000=none) */
            /* 0x0a: padding */
            /* 0x0c: ADPCM offset (from channel info offset), 0xFFFFFFFF otherwise */
            /* 0x10: null? */

            if (read_u16(chnf_offset + 0x00, sf) != 0x1F00)
                goto fail;
            chdt_offset = read_u32(chnf_offset + 0x04, sf) + data_offset + 0x08;

            vgmstream->ch[ch].channel_start_offset = chdt_offset;
            vgmstream->ch[ch].offset = chdt_offset;

            switch(codec) {
                case 0x02: {
                    /* standard DSP coef + predictor + hists + loop predictor + loop hists */
                    uint32_t coef_offset = read_u32(chnf_offset + 0x0c, sf) + chnf_offset;

                    for (i = 0; i < 16; i++) {
                        vgmstream->ch[ch].adpcm_coef[i] = read_s16(coef_offset + 0x00 + i*0x02, sf);
                    }
                    vgmstream->ch[ch].adpcm_history1_16 = read_s16(coef_offset + 0x22, sf);
                    vgmstream->ch[ch].adpcm_history2_16 = read_s16(coef_offset + 0x24, sf);
                    break;
                }

                case 0x03: {
                    /* hist + step */
                    uint32_t coef_offset = read_u32(chnf_offset + 0x0c, sf) + chnf_offset;

                    vgmstream->ch[ch].adpcm_history1_16 = read_s16(coef_offset + 0x00, sf);
                    vgmstream->ch[ch].adpcm_step_index = read_s16(coef_offset + 0x02, sf);
                    break;
                }

                default:
                    break;
            }
        }
    }

    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
