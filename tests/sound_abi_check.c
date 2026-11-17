/* Compile-only proof that arch/x86_64/sound_abi.h matches Linux <sound/asound.h> for x86-64 userspace.
   Build: zig cc -target x86_64-linux-gnu -fsyntax-only tests/sound_abi_check.c */
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <sound/tlv.h>
#include "../arch/x86_64/sound_abi.h"

#define SAME_SIZE(a, b) _Static_assert(sizeof(struct a) == sizeof(struct b), #a " size")
#define SAME_FIELD(a, fa, b, fb) _Static_assert(offsetof(struct a, fa) == offsetof(struct b, fb) && \
    sizeof(((struct a*)0)->fa) == sizeof(((struct b*)0)->fb), #a "." #fa)
#define SAME_IOCTL(a, b) _Static_assert((unsigned)(a) == (unsigned)(b), #a)

SAME_SIZE(nx_interval, snd_interval); SAME_SIZE(nx_mask, snd_mask);
SAME_SIZE(nx_hw_params, snd_pcm_hw_params);
SAME_FIELD(nx_interval, min, snd_interval, min); SAME_FIELD(nx_interval, max, snd_interval, max);
SAME_FIELD(nx_hw_params, flags, snd_pcm_hw_params, flags); SAME_FIELD(nx_hw_params, masks, snd_pcm_hw_params, masks);
SAME_FIELD(nx_hw_params, mres, snd_pcm_hw_params, mres); SAME_FIELD(nx_hw_params, intervals, snd_pcm_hw_params, intervals);
SAME_FIELD(nx_hw_params, ires, snd_pcm_hw_params, ires); SAME_FIELD(nx_hw_params, rmask, snd_pcm_hw_params, rmask);
SAME_FIELD(nx_hw_params, cmask, snd_pcm_hw_params, cmask); SAME_FIELD(nx_hw_params, info, snd_pcm_hw_params, info);
SAME_FIELD(nx_hw_params, msbits, snd_pcm_hw_params, msbits); SAME_FIELD(nx_hw_params, rate_num, snd_pcm_hw_params, rate_num);
SAME_FIELD(nx_hw_params, rate_den, snd_pcm_hw_params, rate_den); SAME_FIELD(nx_hw_params, fifo_size, snd_pcm_hw_params, fifo_size);
SAME_FIELD(nx_hw_params, sync, snd_pcm_hw_params, sync); SAME_FIELD(nx_hw_params, reserved, snd_pcm_hw_params, reserved);

SAME_SIZE(nx_sw_params, snd_pcm_sw_params);
SAME_FIELD(nx_sw_params, tstamp_mode, snd_pcm_sw_params, tstamp_mode); SAME_FIELD(nx_sw_params, period_step, snd_pcm_sw_params, period_step);
SAME_FIELD(nx_sw_params, sleep_min, snd_pcm_sw_params, sleep_min); SAME_FIELD(nx_sw_params, avail_min, snd_pcm_sw_params, avail_min);
SAME_FIELD(nx_sw_params, xfer_align, snd_pcm_sw_params, xfer_align); SAME_FIELD(nx_sw_params, start_threshold, snd_pcm_sw_params, start_threshold);
SAME_FIELD(nx_sw_params, stop_threshold, snd_pcm_sw_params, stop_threshold);
SAME_FIELD(nx_sw_params, silence_threshold, snd_pcm_sw_params, silence_threshold);
SAME_FIELD(nx_sw_params, silence_size, snd_pcm_sw_params, silence_size); SAME_FIELD(nx_sw_params, boundary, snd_pcm_sw_params, boundary);
SAME_FIELD(nx_sw_params, proto, snd_pcm_sw_params, proto); SAME_FIELD(nx_sw_params, tstamp_type, snd_pcm_sw_params, tstamp_type);
SAME_FIELD(nx_sw_params, reserved, snd_pcm_sw_params, reserved);

SAME_SIZE(nx_pcm_info, snd_pcm_info);
SAME_FIELD(nx_pcm_info, device, snd_pcm_info, device); SAME_FIELD(nx_pcm_info, subdevice, snd_pcm_info, subdevice);
SAME_FIELD(nx_pcm_info, stream, snd_pcm_info, stream); SAME_FIELD(nx_pcm_info, card, snd_pcm_info, card);
SAME_FIELD(nx_pcm_info, id, snd_pcm_info, id); SAME_FIELD(nx_pcm_info, name, snd_pcm_info, name);
SAME_FIELD(nx_pcm_info, subname, snd_pcm_info, subname); SAME_FIELD(nx_pcm_info, dev_class, snd_pcm_info, dev_class);
SAME_FIELD(nx_pcm_info, dev_subclass, snd_pcm_info, dev_subclass);
SAME_FIELD(nx_pcm_info, subdevices_count, snd_pcm_info, subdevices_count);
SAME_FIELD(nx_pcm_info, subdevices_avail, snd_pcm_info, subdevices_avail);
SAME_FIELD(nx_pcm_info, pad1, snd_pcm_info, pad1); SAME_FIELD(nx_pcm_info, reserved, snd_pcm_info, reserved);

SAME_SIZE(nx_pcm_status, snd_pcm_status);
SAME_FIELD(nx_pcm_status, state, snd_pcm_status, state); SAME_FIELD(nx_pcm_status, trigger_tstamp, snd_pcm_status, trigger_tstamp);
SAME_FIELD(nx_pcm_status, tstamp, snd_pcm_status, tstamp); SAME_FIELD(nx_pcm_status, appl_ptr, snd_pcm_status, appl_ptr);
SAME_FIELD(nx_pcm_status, hw_ptr, snd_pcm_status, hw_ptr); SAME_FIELD(nx_pcm_status, delay, snd_pcm_status, delay);
SAME_FIELD(nx_pcm_status, avail, snd_pcm_status, avail); SAME_FIELD(nx_pcm_status, avail_max, snd_pcm_status, avail_max);
SAME_FIELD(nx_pcm_status, overrange, snd_pcm_status, overrange);
SAME_FIELD(nx_pcm_status, suspended_state, snd_pcm_status, suspended_state);
SAME_FIELD(nx_pcm_status, audio_tstamp_data, snd_pcm_status, audio_tstamp_data);
SAME_FIELD(nx_pcm_status, audio_tstamp, snd_pcm_status, audio_tstamp);
SAME_FIELD(nx_pcm_status, driver_tstamp, snd_pcm_status, driver_tstamp);
SAME_FIELD(nx_pcm_status, audio_tstamp_accuracy, snd_pcm_status, audio_tstamp_accuracy);
SAME_FIELD(nx_pcm_status, reserved, snd_pcm_status, reserved);
SAME_SIZE(nx_timespec, timespec);
SAME_FIELD(nx_timespec, sec, timespec, tv_sec); SAME_FIELD(nx_timespec, nsec, timespec, tv_nsec);

SAME_SIZE(nx_mmap_status, snd_pcm_mmap_status);
SAME_FIELD(nx_mmap_status, state, snd_pcm_mmap_status, state); SAME_FIELD(nx_mmap_status, hw_ptr, snd_pcm_mmap_status, hw_ptr);
SAME_FIELD(nx_mmap_status, tstamp, snd_pcm_mmap_status, tstamp);
SAME_FIELD(nx_mmap_status, suspended_state, snd_pcm_mmap_status, suspended_state);
SAME_FIELD(nx_mmap_status, audio_tstamp, snd_pcm_mmap_status, audio_tstamp);
SAME_SIZE(nx_mmap_control, snd_pcm_mmap_control);
SAME_FIELD(nx_mmap_control, appl_ptr, snd_pcm_mmap_control, appl_ptr);
SAME_FIELD(nx_mmap_control, avail_min, snd_pcm_mmap_control, avail_min);
SAME_SIZE(nx_sync_ptr, snd_pcm_sync_ptr);
SAME_FIELD(nx_sync_ptr, flags, snd_pcm_sync_ptr, flags); SAME_FIELD(nx_sync_ptr, s, snd_pcm_sync_ptr, s);
SAME_FIELD(nx_sync_ptr, c, snd_pcm_sync_ptr, c);
SAME_FIELD(nx_sync_ptr, s.status.hw_ptr, snd_pcm_sync_ptr, s.status.hw_ptr);
SAME_FIELD(nx_sync_ptr, c.control.avail_min, snd_pcm_sync_ptr, c.control.avail_min);

SAME_SIZE(nx_channel_info, snd_pcm_channel_info);
SAME_FIELD(nx_channel_info, channel, snd_pcm_channel_info, channel); SAME_FIELD(nx_channel_info, offset, snd_pcm_channel_info, offset);
SAME_FIELD(nx_channel_info, first, snd_pcm_channel_info, first); SAME_FIELD(nx_channel_info, step, snd_pcm_channel_info, step);
SAME_SIZE(nx_xferi, snd_xferi);
SAME_FIELD(nx_xferi, result, snd_xferi, result); SAME_FIELD(nx_xferi, buffer, snd_xferi, buf);
SAME_FIELD(nx_xferi, frames, snd_xferi, frames);

SAME_SIZE(nx_card_info, snd_ctl_card_info);
SAME_FIELD(nx_card_info, card, snd_ctl_card_info, card); SAME_FIELD(nx_card_info, id, snd_ctl_card_info, id);
SAME_FIELD(nx_card_info, driver, snd_ctl_card_info, driver); SAME_FIELD(nx_card_info, name, snd_ctl_card_info, name);
SAME_FIELD(nx_card_info, longname, snd_ctl_card_info, longname); SAME_FIELD(nx_card_info, reserved, snd_ctl_card_info, reserved_);
SAME_FIELD(nx_card_info, mixername, snd_ctl_card_info, mixername);
SAME_FIELD(nx_card_info, components, snd_ctl_card_info, components);
SAME_SIZE(nx_elem_list, snd_ctl_elem_list);
SAME_FIELD(nx_elem_list, offset, snd_ctl_elem_list, offset); SAME_FIELD(nx_elem_list, space, snd_ctl_elem_list, space);
SAME_FIELD(nx_elem_list, used, snd_ctl_elem_list, used); SAME_FIELD(nx_elem_list, count, snd_ctl_elem_list, count);
SAME_FIELD(nx_elem_list, pids, snd_ctl_elem_list, pids);

SAME_IOCTL(NX_PCM_IOCTL_PVERSION, SNDRV_PCM_IOCTL_PVERSION); SAME_IOCTL(NX_PCM_IOCTL_INFO, SNDRV_PCM_IOCTL_INFO);
SAME_IOCTL(NX_PCM_IOCTL_TSTAMP, SNDRV_PCM_IOCTL_TSTAMP); SAME_IOCTL(NX_PCM_IOCTL_TTSTAMP, SNDRV_PCM_IOCTL_TTSTAMP);
SAME_IOCTL(NX_PCM_IOCTL_USER_PVERSION, SNDRV_PCM_IOCTL_USER_PVERSION);
SAME_IOCTL(NX_PCM_IOCTL_HW_REFINE, SNDRV_PCM_IOCTL_HW_REFINE); SAME_IOCTL(NX_PCM_IOCTL_HW_PARAMS, SNDRV_PCM_IOCTL_HW_PARAMS);
SAME_IOCTL(NX_PCM_IOCTL_HW_FREE, SNDRV_PCM_IOCTL_HW_FREE); SAME_IOCTL(NX_PCM_IOCTL_SW_PARAMS, SNDRV_PCM_IOCTL_SW_PARAMS);
SAME_IOCTL(NX_PCM_IOCTL_STATUS, SNDRV_PCM_IOCTL_STATUS); SAME_IOCTL(NX_PCM_IOCTL_DELAY, SNDRV_PCM_IOCTL_DELAY);
SAME_IOCTL(NX_PCM_IOCTL_HWSYNC, SNDRV_PCM_IOCTL_HWSYNC); SAME_IOCTL(NX_PCM_IOCTL_SYNC_PTR, SNDRV_PCM_IOCTL_SYNC_PTR);
SAME_IOCTL(NX_PCM_IOCTL_STATUS_EXT, SNDRV_PCM_IOCTL_STATUS_EXT);
SAME_IOCTL(NX_PCM_IOCTL_CHANNEL_INFO, SNDRV_PCM_IOCTL_CHANNEL_INFO);
SAME_IOCTL(NX_PCM_IOCTL_PREPARE, SNDRV_PCM_IOCTL_PREPARE); SAME_IOCTL(NX_PCM_IOCTL_RESET, SNDRV_PCM_IOCTL_RESET);
SAME_IOCTL(NX_PCM_IOCTL_START, SNDRV_PCM_IOCTL_START); SAME_IOCTL(NX_PCM_IOCTL_DROP, SNDRV_PCM_IOCTL_DROP);
SAME_IOCTL(NX_PCM_IOCTL_DRAIN, SNDRV_PCM_IOCTL_DRAIN); SAME_IOCTL(NX_PCM_IOCTL_PAUSE, SNDRV_PCM_IOCTL_PAUSE);
SAME_IOCTL(NX_PCM_IOCTL_REWIND, SNDRV_PCM_IOCTL_REWIND); SAME_IOCTL(NX_PCM_IOCTL_RESUME, SNDRV_PCM_IOCTL_RESUME);
SAME_IOCTL(NX_PCM_IOCTL_XRUN, SNDRV_PCM_IOCTL_XRUN); SAME_IOCTL(NX_PCM_IOCTL_FORWARD, SNDRV_PCM_IOCTL_FORWARD);
SAME_IOCTL(NX_PCM_IOCTL_WRITEI_FRAMES, SNDRV_PCM_IOCTL_WRITEI_FRAMES);
SAME_IOCTL(NX_PCM_IOCTL_LINK, SNDRV_PCM_IOCTL_LINK); SAME_IOCTL(NX_PCM_IOCTL_UNLINK, SNDRV_PCM_IOCTL_UNLINK);
SAME_IOCTL(NX_CTL_IOCTL_PVERSION, SNDRV_CTL_IOCTL_PVERSION); SAME_IOCTL(NX_CTL_IOCTL_CARD_INFO, SNDRV_CTL_IOCTL_CARD_INFO);
SAME_IOCTL(NX_CTL_IOCTL_ELEM_LIST, SNDRV_CTL_IOCTL_ELEM_LIST);
SAME_IOCTL(NX_CTL_IOCTL_SUBSCRIBE_EVENTS, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS);
SAME_IOCTL(NX_CTL_IOCTL_HWDEP_NEXT_DEVICE, SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE);
SAME_IOCTL(NX_CTL_IOCTL_PCM_NEXT_DEVICE, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE);
SAME_IOCTL(NX_CTL_IOCTL_PCM_INFO, SNDRV_CTL_IOCTL_PCM_INFO);
SAME_IOCTL(NX_CTL_IOCTL_PCM_PREFER_SUBDEVICE, SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE);
SAME_IOCTL(NX_CTL_IOCTL_RAWMIDI_NEXT_DEVICE, SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE);
SAME_IOCTL(NX_CTL_IOCTL_UMP_NEXT_DEVICE, SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE);
SAME_IOCTL(NX_CTL_IOCTL_POWER_STATE, SNDRV_CTL_IOCTL_POWER_STATE);

_Static_assert(NX_PCM_VERSION == SNDRV_PCM_VERSION && NX_CTL_VERSION == SNDRV_CTL_VERSION, "protocol versions");
_Static_assert(NX_PARAM_ACCESS == SNDRV_PCM_HW_PARAM_ACCESS && NX_PARAM_SUBFORMAT == SNDRV_PCM_HW_PARAM_SUBFORMAT &&
               NX_PARAM_SAMPLE_BITS == SNDRV_PCM_HW_PARAM_SAMPLE_BITS && NX_PARAM_RATE == SNDRV_PCM_HW_PARAM_RATE &&
               NX_PARAM_PERIODS == SNDRV_PCM_HW_PARAM_PERIODS && NX_PARAM_BUFFER_BYTES == SNDRV_PCM_HW_PARAM_BUFFER_BYTES &&
               NX_PARAM_TICK_TIME == SNDRV_PCM_HW_PARAM_TICK_TIME, "hw parameter indices");
_Static_assert(NX_STATE_OPEN == SNDRV_PCM_STATE_OPEN && NX_STATE_PREPARED == SNDRV_PCM_STATE_PREPARED &&
               NX_STATE_XRUN == SNDRV_PCM_STATE_XRUN && NX_STATE_DRAINING == SNDRV_PCM_STATE_DRAINING &&
               NX_STATE_PAUSED == SNDRV_PCM_STATE_PAUSED, "stream states");
_Static_assert(NX_ACCESS_MMAP_INTERLEAVED == SNDRV_PCM_ACCESS_MMAP_INTERLEAVED && NX_ACCESS_RW_INTERLEAVED == SNDRV_PCM_ACCESS_RW_INTERLEAVED &&
               NX_FORMAT_S16_LE == SNDRV_PCM_FORMAT_S16_LE, "access/format numbers");
_Static_assert(NX_INFO_MMAP == SNDRV_PCM_INFO_MMAP && NX_INFO_MMAP_VALID == SNDRV_PCM_INFO_MMAP_VALID &&
               NX_INFO_PERFECT_DRAIN == SNDRV_PCM_INFO_PERFECT_DRAIN && NX_INFO_INTERLEAVED == SNDRV_PCM_INFO_INTERLEAVED &&
               NX_INFO_BLOCK_TRANSFER == SNDRV_PCM_INFO_BLOCK_TRANSFER && NX_INFO_PAUSE == SNDRV_PCM_INFO_PAUSE, "info bits");
_Static_assert(NX_SYNC_PTR_HWSYNC == SNDRV_PCM_SYNC_PTR_HWSYNC && NX_SYNC_PTR_APPL == SNDRV_PCM_SYNC_PTR_APPL &&
               NX_SYNC_PTR_AVAIL_MIN == SNDRV_PCM_SYNC_PTR_AVAIL_MIN, "sync_ptr flags");
_Static_assert(SNDRV_PCM_MMAP_OFFSET_DATA == 0 && SNDRV_PCM_MMAP_OFFSET_STATUS == 0x80000000U, "mmap offsets");

/* Capture and mixer controls (2026-10-19). */
SAME_IOCTL(NX_PCM_IOCTL_READI_FRAMES, SNDRV_PCM_IOCTL_READI_FRAMES);
SAME_SIZE(nx_elem_id, snd_ctl_elem_id); SAME_SIZE(nx_elem_info, snd_ctl_elem_info); SAME_SIZE(nx_elem_value, snd_ctl_elem_value);
SAME_FIELD(nx_elem_id, numid, snd_ctl_elem_id, numid); SAME_FIELD(nx_elem_id, iface, snd_ctl_elem_id, iface);
SAME_FIELD(nx_elem_id, name, snd_ctl_elem_id, name); SAME_FIELD(nx_elem_id, index, snd_ctl_elem_id, index);
SAME_FIELD(nx_elem_info, type, snd_ctl_elem_info, type); SAME_FIELD(nx_elem_info, access, snd_ctl_elem_info, access);
SAME_FIELD(nx_elem_info, count, snd_ctl_elem_info, count); SAME_FIELD(nx_elem_info, owner, snd_ctl_elem_info, owner);
SAME_FIELD(nx_elem_info, value.integer.min, snd_ctl_elem_info, value.integer.min);
SAME_FIELD(nx_elem_info, value.integer.max, snd_ctl_elem_info, value.integer.max);
SAME_FIELD(nx_elem_info, value.integer.step, snd_ctl_elem_info, value.integer.step);
SAME_FIELD(nx_elem_value, value.integer, snd_ctl_elem_value, value.integer.value);
SAME_FIELD(nx_elem_value, reserved, snd_ctl_elem_value, reserved);
SAME_IOCTL(NX_CTL_IOCTL_ELEM_INFO, SNDRV_CTL_IOCTL_ELEM_INFO); SAME_IOCTL(NX_CTL_IOCTL_ELEM_READ, SNDRV_CTL_IOCTL_ELEM_READ);
SAME_IOCTL(NX_CTL_IOCTL_ELEM_WRITE, SNDRV_CTL_IOCTL_ELEM_WRITE); SAME_IOCTL(NX_CTL_IOCTL_ELEM_LOCK, SNDRV_CTL_IOCTL_ELEM_LOCK);
SAME_IOCTL(NX_CTL_IOCTL_ELEM_UNLOCK, SNDRV_CTL_IOCTL_ELEM_UNLOCK); SAME_IOCTL(NX_CTL_IOCTL_TLV_READ, SNDRV_CTL_IOCTL_TLV_READ);
_Static_assert(NX_ELEM_IFACE_MIXER == SNDRV_CTL_ELEM_IFACE_MIXER && NX_ELEM_ACCESS_READWRITE == SNDRV_CTL_ELEM_ACCESS_READWRITE &&
               NX_ELEM_ACCESS_TLV_READ == SNDRV_CTL_ELEM_ACCESS_TLV_READ && SNDRV_CTL_ELEM_TYPE_BOOLEAN == 1 && SNDRV_CTL_ELEM_TYPE_INTEGER == 2 &&
               NX_TLVT_DB_SCALE == SNDRV_CTL_TLVT_DB_SCALE && sizeof(struct snd_ctl_tlv) == sizeof(struct nx_ctl_tlv), "control constants");
