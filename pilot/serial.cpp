#include "serial.h"
#include "metrics.h"
#include "crc.h"
#include "../teensy_rc_control/Packets.h"

#include <unistd.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <assert.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <linux/input.h>
#include <linux/hidraw.h>


static int hidport = -1;
static pthread_t hidthread;
static bool hidRunning;
static uint64_t lastHidSendTime;
static uint8_t my_frame_id = 0;
static uint8_t peer_frameid = 0;

/* microseconds */
#define HID_SEND_INTERVAL 16384

static char const *charstr(int i, char *str) {
    if (i >= 32 && i < 127) {
        str[0] = i;
        str[1] = 0;
        return str;
    }
    str[0] = '.';
    str[1] = 0;
    return str;
}


static void hexdump(void const *vp, int end) {
    unsigned char const *p = (unsigned char const *)vp;
    int offset = 0;
    char s[10];
    while (offset < end) {
        fprintf(stderr, "%04x: ", offset);
        for (int i = 0; i != 16; ++i) {
            if (i+offset < end) {
                fprintf(stderr, " %02x", p[i+offset]);
            } else {
                fprintf(stderr, "   ");
            }
        }
        fprintf(stderr, " ");
        for (int i = 0; i != 16; ++i) {
            if (i+offset < end) {
                fprintf(stderr, "%s", charstr(p[i+offset], s));
            } else {
                fprintf(stderr, " ");
            }
        }
        fprintf(stderr, "\n");
        offset += 16;
    }
}

template<typename T> struct IncomingPacket {
    T data;
    bool fresh;
    uint64_t when;
};

static IncomingPacket<TrimInfo> g_TrimInfo;
static IncomingPacket<IBusPacket> g_IBusPacket;
static IncomingPacket<SteerControl> g_SteerControl;

TrimInfo const *serial_trim_info(uint64_t *oTime, bool *oFresh) {
    *oFresh = g_TrimInfo.fresh;
    if (g_TrimInfo.fresh) g_TrimInfo.fresh = false;
    *oTime = g_TrimInfo.when;
    return &g_TrimInfo.data;
}

IBusPacket const *serial_ibus_packet(uint64_t *oTime, bool *oFresh) {
    *oFresh = g_IBusPacket.fresh;
    if (g_IBusPacket.fresh) g_IBusPacket.fresh = false;
    *oTime = g_IBusPacket.when;
    return &g_IBusPacket.data;
}

SteerControl const *serial_steer_control(uint64_t *oTime, bool *oFresh) {
    *oFresh = g_SteerControl.fresh;
    if (g_SteerControl.fresh) g_SteerControl.fresh = false;
    *oTime = g_SteerControl.when;
    return &g_SteerControl.data;
}


#define RECVPACKET(type) \
    case type::PacketCode: \
        if (!unpack(g_##type, sizeof(type), ++buf, end)) { \
            fprintf(stderr, "Packet too short for %s: %ld\n", #type, (long)(end-buf)); \
            hexdump(buf, end-buf); \
            Serial_UnknownMessage.set(); \
            return; \
        } \
        break;

template<typename T> bool unpack(IncomingPacket<T> &dst, size_t dsz, unsigned char const *&src, unsigned char const *end) {
    if (end-src < (long)dsz) {
        return false;
    }
    memcpy(&dst.data, src, dsz);
    src += dsz;
    dst.fresh = true;
    dst.when = metric::Collector::clock();
    return true;
}

// buf points at frameid
static void handle_packet(unsigned char const *buf, unsigned char const *end) {
    assert(end - buf >= 5);
    uint16_t crc = crc_kermit(buf+2, end-buf-4);
    if (((crc & 0xff) != end[-2]) || (((crc >> 8) & 0xff) != end[-1])) {
        Serial_CrcErrors.increment();
        fprintf(stderr, "serial CRC got %x calculated %x\n", end[-2] | (end[-1] << 8), crc);
        return;
    }
    peer_frameid = buf[0];
    //  ignore lastseen for now
    //  ignore length; already handled by caller
    buf += 3;
    end -= 2;
    while (buf != end) {
        //  decode a packet
        switch (*buf) {
            RECVPACKET(TrimInfo);
            RECVPACKET(IBusPacket);
            RECVPACKET(SteerControl);
            default:
                fprintf(stderr, "serial unknown message ID 0x%02x; flushing to end of packet\n", *buf);
                hexdump(buf, end-buf);
                Serial_UnknownMessage.set();
                return;
        }
        Serial_PacketsDecoded.increment();
    }
}

/* 0x55 0xAA <frameid> <lastseen> <length> <payload> <CRC16-L> <CRC16-H>
 * CRC16 is CRC16 of <length> and <payload>.
 */
static void parse_buffer(unsigned char *buf, int &bufptr) {
    if (bufptr < 7) {
        fprintf(stderr, "short buffer: %d bytes\n", bufptr);
        return; //  can't be anything
    }
    if (buf[0] == 0x55 && buf[1] == 0xAA) {
        //  header
        if (buf[4] > 64-7) {
            //  must flush this packet
            goto clear_to_i;
        }
        if (buf[4] + 7 > bufptr) {
            //  don't yet have all the data
            goto clear_to_i;
        }
        handle_packet(buf + 2, buf + 7 + buf[4]);
        return; //  success
    }
clear_to_i:
    fprintf(stderr, "bufptr %d failure\n", bufptr);
    return; //  failure
}

static bool send_outgoing_packet() {
    ++my_frame_id;
    if (!my_frame_id) {
        my_frame_id = 1;
    }
    unsigned char pack[64] = { 0x55, 0xAA, my_frame_id, peer_frameid, 0 };
    int dlen = 0, dmaxlen = 64-7;
    // pack in some stuff
    (void)&dmaxlen,(void)&dlen;
    pack[4] = (unsigned char)(dlen & 0xff);
    uint16_t crc = crc_kermit(&pack[4], dlen+1);
    pack[5+dlen] = (unsigned char)(crc & 0xff);
    pack[6+dlen] = (unsigned char)((crc >> 8) & 0xff);
    int wr = ::write(hidport, pack, dlen+7);
    if (wr != dlen+7) {
        if (wr < 0) {
            perror("serial write");
        } else {
            fprintf(stderr, "serial short write: %d instead of %d\n", wr, dlen+7);
        }
        return false;
    }
    Serial_BytesSent.increment(wr);
    return true;
}

static void *ser_fn(void *) {
    int bufptr = 0;
    unsigned char inbuf[64] = { 0 };
    int nerror = 0;
    puts("start hid thread");
    bool shouldsleep = false;
    while (hidRunning) {
        if (bufptr == 256) {
            bufptr = 0; //  flush
        }
        uint64_t now = metric::Collector::clock();
        int n = 0;
        if (shouldsleep) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(hidport, &fds);
            struct timeval tv = { 0, 0 };
            tv.tv_usec = HID_SEND_INTERVAL - (now - lastHidSendTime);
            if (tv.tv_usec < 1) {
                tv.tv_usec = 1;
            }
            if (tv.tv_usec > 20000) {
                tv.tv_usec = 20000;
            }
            n = select(hidport+1, &fds, NULL, NULL, &tv);
            now = metric::Collector::clock();
        }
        if (n >= 0) {
            if (now - lastHidSendTime >= HID_SEND_INTERVAL) {
                //  quantize to the send interval
                lastHidSendTime = now - (now % HID_SEND_INTERVAL);
                //  do send
                if (!send_outgoing_packet()) {
                    puts("send error");
                    Serial_Error.set();
                    ++nerror;
                    if (nerror >= 10) {
                        break;
                    }
                }
            }
            int r = read(hidport, inbuf, sizeof(inbuf));
            if (r < 0) {
                shouldsleep = true;
                if (errno != EAGAIN) {
                    if (hidRunning) {
                        Serial_Error.set();
                        perror("serial");
                        ++nerror;
                        if (nerror >= 10) {
                            break;
                        }
                    }
                }
            } else {
                shouldsleep = false;
                if (r > 0) {
                    Serial_BytesReceived.increment(r);
                    if (nerror) {
                        --nerror;
                    }
                    bufptr += r;
                }
                if (bufptr > 0) {
                    //  parse and perhaps remove data
                    parse_buffer(inbuf, bufptr);
                    bufptr = 0;
                }
            }
        } else {
            perror("select");
            ++nerror;
            if (nerror >= 10) {
                break;
            }
        }
    }
    puts("end hidthread");
    return 0;
}

#define TEENSY_VENDOR 0x16c0
#define TEENSY_PRODUCT 0x0486
#define TEENSY_RAWHID_ENDPOINT_DESC_SIZE 28

static int verify_open(char const *name) {
    int fd;
    int sz[2] = { 0 };
    hidraw_devinfo hrdi = { 0 };

    fd = open(name, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (ioctl(fd, HIDIOCGRAWINFO, &hrdi) < 0) {
        goto bad_fd;
    }
    if (hrdi.vendor != TEENSY_VENDOR || hrdi.product != TEENSY_PRODUCT) {
        goto bad_fd;
    }
    if (ioctl(fd, HIDIOCGRDESCSIZE, sz) < 0) {
        goto bad_fd;
    }
    //  A magical size found by examination
    if (sz[0] != TEENSY_RAWHID_ENDPOINT_DESC_SIZE) {
        goto bad_fd;
    }
    fprintf(stderr, "serial/hidraw open %s returns fd %d\n", name, fd);
    //  I know this is the right fd, because if I write "reset!" to it, 
    //  the Teensy resets, like it should.
    return fd;

bad_fd:
    if (fd >= 0) {
        close(fd);
    }
    return -1;
}

bool start_serial(char const *port, int speed) {
    if (hidthread) {
        return false;
    }
    if (hidport != -1) {
        return false;
    }

    hidport = verify_open(port);
    if (hidport < 0) {
        for (int i = 0; i != 99; ++i) {
            char buf[30];
            sprintf(buf, "/dev/hidraw%d", i);
            hidport = verify_open(buf);
            if (hidport >= 0) {
                break;
            }
        }
        if (hidport < 0) {
            fprintf(stderr, "%s is not a recognized hidraw device, and couldn't find one through scanning.\n", port);
            return false;
        }
    }

    hidRunning = true;
    if (pthread_create(&hidthread, NULL, ser_fn, NULL) < 0) {
        close(hidport);
        hidport = -1;
        hidRunning = false;
        return false;
    }
    return true;
}

void stop_serial() {
    if (hidthread) {
        hidRunning = false;
        close(hidport);
        void *x = 0;
        pthread_join(hidthread, &x);
        hidport = -1;
        hidthread = 0;
    }
}


