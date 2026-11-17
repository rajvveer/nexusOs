/* Linux ALSA kernel ABI (x86-64 userspace layout) used by /dev/snd; checked by tests/sound_abi_check.c. */
#ifndef NEXUS_SOUND_ABI_H
#define NEXUS_SOUND_ABI_H

#define NX_PROTOCOL(major, minor, sub) (((major) << 16) | ((minor) << 8) | (sub))
#define NX_PCM_VERSION NX_PROTOCOL(2, 0, 18)
#define NX_CTL_VERSION NX_PROTOCOL(2, 0, 9)
#define NX_IOC(dir, type, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr))
#define NX_IO(type, nr) NX_IOC(0, type, nr, 0)
#define NX_IOW(type, nr, size) NX_IOC(1, type, nr, size)
#define NX_IOR(type, nr, size) NX_IOC(2, type, nr, size)
#define NX_IOWR(type, nr, size) NX_IOC(3, type, nr, size)

enum { NX_PARAM_ACCESS, NX_PARAM_FORMAT, NX_PARAM_SUBFORMAT, NX_PARAM_SAMPLE_BITS = 8, NX_PARAM_FRAME_BITS, NX_PARAM_CHANNELS,
       NX_PARAM_RATE, NX_PARAM_PERIOD_TIME, NX_PARAM_PERIOD_SIZE, NX_PARAM_PERIOD_BYTES, NX_PARAM_PERIODS, NX_PARAM_BUFFER_TIME,
       NX_PARAM_BUFFER_SIZE, NX_PARAM_BUFFER_BYTES, NX_PARAM_TICK_TIME };
enum { NX_STATE_OPEN, NX_STATE_SETUP, NX_STATE_PREPARED, NX_STATE_RUNNING, NX_STATE_XRUN, NX_STATE_DRAINING, NX_STATE_PAUSED,
       NX_STATE_SUSPENDED, NX_STATE_DISCONNECTED };
#define NX_ACCESS_MMAP_INTERLEAVED 0
#define NX_ACCESS_RW_INTERLEAVED 3
#define NX_FORMAT_S16_LE 2
#define NX_INFO_MMAP 0x1U
#define NX_INFO_MMAP_VALID 0x2U
#define NX_INFO_PERFECT_DRAIN 0x40U
#define NX_INFO_INTERLEAVED 0x100U
#define NX_INFO_BLOCK_TRANSFER 0x10000U
#define NX_INFO_PAUSE 0x80000U
#define NX_SYNC_PTR_HWSYNC 1U
#define NX_SYNC_PTR_APPL 2U
#define NX_SYNC_PTR_AVAIL_MIN 4U

struct nx_interval { uint32_t min, max; uint32_t openmin : 1, openmax : 1, integer : 1, empty : 1; };
struct nx_mask { uint32_t bits[8]; };
struct nx_hw_params {
    uint32_t flags; struct nx_mask masks[3], mres[5]; struct nx_interval intervals[12], ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den; uint64_t fifo_size;
    uint8_t sync[16], reserved[48];
};
struct nx_sw_params {
    int32_t tstamp_mode; uint32_t period_step, sleep_min;
    uint64_t avail_min, xfer_align, start_threshold, stop_threshold, silence_threshold, silence_size, boundary;
    uint32_t proto, tstamp_type; uint8_t reserved[56];
};
struct nx_pcm_info {
    uint32_t device, subdevice; int32_t stream, card; uint8_t id[64], name[80], subname[32];
    int32_t dev_class, dev_subclass; uint32_t subdevices_count, subdevices_avail; uint8_t pad1[16], reserved[64];
};
struct nx_timespec { int64_t sec, nsec; };
struct nx_pcm_status {
    int32_t state, pad1; struct nx_timespec trigger_tstamp, tstamp;
    uint64_t appl_ptr, hw_ptr; int64_t delay; uint64_t avail, avail_max, overrange;
    int32_t suspended_state; uint32_t audio_tstamp_data; struct nx_timespec audio_tstamp, driver_tstamp;
    uint32_t audio_tstamp_accuracy; uint8_t reserved[20];
};
struct nx_mmap_status { int32_t state, pad1; uint64_t hw_ptr; struct nx_timespec tstamp; int32_t suspended_state, pad2; struct nx_timespec audio_tstamp; };
struct nx_mmap_control { uint64_t appl_ptr, avail_min; };
struct nx_sync_ptr {
    uint32_t flags, pad1;
    union { struct nx_mmap_status status; uint8_t reserved[64]; } s;
    union { struct nx_mmap_control control; uint8_t reserved[64]; } c;
};
struct nx_channel_info { uint32_t channel, pad; int64_t offset; uint32_t first, step; };
struct nx_xferi { int64_t result; uint64_t buffer, frames; };
struct nx_card_info {
    int32_t card, pad; uint8_t id[16], driver[16], name[32], longname[80], reserved[16], mixername[80], components[128];
};
struct nx_elem_list { uint32_t offset, space, used, count; uint64_t pids; uint8_t reserved[50]; };
/* Control elements (mixer): snd_ctl_elem_id / _info / _value and snd_ctl_tlv. */
struct nx_elem_id { uint32_t numid; int32_t iface; uint32_t device, subdevice; uint8_t name[44]; uint32_t index; };
struct nx_elem_info {
    struct nx_elem_id id; int32_t type; uint32_t access, count; int32_t owner;
    union { struct { int64_t min, max, step; } integer; uint8_t reserved[128]; } value;
    uint8_t reserved[64];
};
struct nx_elem_value { struct nx_elem_id id; uint32_t indirect, pad; union { int64_t integer[128]; uint8_t reserved[1024]; } value; uint8_t reserved[128]; };
struct nx_ctl_tlv { uint32_t numid, length; };
#define NX_ELEM_IFACE_MIXER 2
#define NX_ELEM_ACCESS_READWRITE 3U
#define NX_ELEM_ACCESS_TLV_READ 0x10U
#define NX_TLVT_DB_SCALE 1U
#define NX_TLVT_DB_MINMAX 4U

#define NX_PCM_IOCTL_PVERSION NX_IOR('A', 0x00, 4)
#define NX_PCM_IOCTL_INFO NX_IOR('A', 0x01, sizeof(struct nx_pcm_info))
#define NX_PCM_IOCTL_TSTAMP NX_IOW('A', 0x02, 4)
#define NX_PCM_IOCTL_TTSTAMP NX_IOW('A', 0x03, 4)
#define NX_PCM_IOCTL_USER_PVERSION NX_IOW('A', 0x04, 4)
#define NX_PCM_IOCTL_HW_REFINE NX_IOWR('A', 0x10, sizeof(struct nx_hw_params))
#define NX_PCM_IOCTL_HW_PARAMS NX_IOWR('A', 0x11, sizeof(struct nx_hw_params))
#define NX_PCM_IOCTL_HW_FREE NX_IO('A', 0x12)
#define NX_PCM_IOCTL_SW_PARAMS NX_IOWR('A', 0x13, sizeof(struct nx_sw_params))
#define NX_PCM_IOCTL_STATUS NX_IOR('A', 0x20, sizeof(struct nx_pcm_status))
#define NX_PCM_IOCTL_DELAY NX_IOR('A', 0x21, 8)
#define NX_PCM_IOCTL_HWSYNC NX_IO('A', 0x22)
#define NX_PCM_IOCTL_SYNC_PTR NX_IOWR('A', 0x23, sizeof(struct nx_sync_ptr))
#define NX_PCM_IOCTL_STATUS_EXT NX_IOWR('A', 0x24, sizeof(struct nx_pcm_status))
#define NX_PCM_IOCTL_CHANNEL_INFO NX_IOR('A', 0x32, sizeof(struct nx_channel_info))
#define NX_PCM_IOCTL_PREPARE NX_IO('A', 0x40)
#define NX_PCM_IOCTL_RESET NX_IO('A', 0x41)
#define NX_PCM_IOCTL_START NX_IO('A', 0x42)
#define NX_PCM_IOCTL_DROP NX_IO('A', 0x43)
#define NX_PCM_IOCTL_DRAIN NX_IO('A', 0x44)
#define NX_PCM_IOCTL_PAUSE NX_IOW('A', 0x45, 4)
#define NX_PCM_IOCTL_REWIND NX_IOW('A', 0x46, 8)
#define NX_PCM_IOCTL_RESUME NX_IO('A', 0x47)
#define NX_PCM_IOCTL_XRUN NX_IO('A', 0x48)
#define NX_PCM_IOCTL_FORWARD NX_IOW('A', 0x49, 8)
#define NX_PCM_IOCTL_WRITEI_FRAMES NX_IOW('A', 0x50, sizeof(struct nx_xferi))
#define NX_PCM_IOCTL_READI_FRAMES NX_IOR('A', 0x51, sizeof(struct nx_xferi))
#define NX_PCM_IOCTL_LINK NX_IOW('A', 0x60, 4)
#define NX_PCM_IOCTL_UNLINK NX_IO('A', 0x61)
#define NX_CTL_IOCTL_PVERSION NX_IOR('U', 0x00, 4)
#define NX_CTL_IOCTL_CARD_INFO NX_IOR('U', 0x01, sizeof(struct nx_card_info))
#define NX_CTL_IOCTL_ELEM_LIST NX_IOWR('U', 0x10, sizeof(struct nx_elem_list))
#define NX_CTL_IOCTL_ELEM_INFO NX_IOWR('U', 0x11, sizeof(struct nx_elem_info))
#define NX_CTL_IOCTL_ELEM_READ NX_IOWR('U', 0x12, sizeof(struct nx_elem_value))
#define NX_CTL_IOCTL_ELEM_WRITE NX_IOWR('U', 0x13, sizeof(struct nx_elem_value))
#define NX_CTL_IOCTL_ELEM_LOCK NX_IOW('U', 0x14, sizeof(struct nx_elem_id))
#define NX_CTL_IOCTL_ELEM_UNLOCK NX_IOW('U', 0x15, sizeof(struct nx_elem_id))
#define NX_CTL_IOCTL_TLV_READ NX_IOWR('U', 0x1A, sizeof(struct nx_ctl_tlv))
#define NX_CTL_IOCTL_SUBSCRIBE_EVENTS NX_IOWR('U', 0x16, 4)
#define NX_CTL_IOCTL_HWDEP_NEXT_DEVICE NX_IOWR('U', 0x20, 4)
#define NX_CTL_IOCTL_PCM_NEXT_DEVICE NX_IOR('U', 0x30, 4)
#define NX_CTL_IOCTL_PCM_INFO NX_IOWR('U', 0x31, sizeof(struct nx_pcm_info))
#define NX_CTL_IOCTL_PCM_PREFER_SUBDEVICE NX_IOW('U', 0x32, 4)
#define NX_CTL_IOCTL_RAWMIDI_NEXT_DEVICE NX_IOWR('U', 0x40, 4)
#define NX_CTL_IOCTL_UMP_NEXT_DEVICE NX_IOWR('U', 0x43, 4)
#define NX_CTL_IOCTL_POWER_STATE NX_IOR('U', 0xD1, 4)

_Static_assert(sizeof(struct nx_hw_params) == 608 && sizeof(struct nx_sw_params) == 136 && sizeof(struct nx_pcm_info) == 288 &&
               sizeof(struct nx_pcm_status) == 152 && sizeof(struct nx_sync_ptr) == 136 && sizeof(struct nx_channel_info) == 24 &&
               sizeof(struct nx_xferi) == 24 && sizeof(struct nx_card_info) == 376 && sizeof(struct nx_elem_list) == 80 &&
               sizeof(struct nx_elem_id) == 64 && sizeof(struct nx_elem_info) == 272 && sizeof(struct nx_elem_value) == 1224 &&
               sizeof(struct nx_ctl_tlv) == 8,
               "x86-64 ALSA ABI sizes");
#endif
