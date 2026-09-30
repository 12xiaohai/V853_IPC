#include "rtsp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include <MediaStream.h>
#include <TinyServer.h>

#define RTSP_SERVER_COUNT 4
#define RTSP_SERVER_PORT 8554

static TinyServer *g_servers[RTSP_SERVER_COUNT];
static MediaStream *g_streams[RTSP_SERVER_COUNT];

static const char *net_type_to_name(RtspNetType type)
{
    switch (type) {
    case RTSP_NET_TYPE_LO:
        return "lo";
    case RTSP_NET_TYPE_ETH0:
        return "eth0";
    case RTSP_NET_TYPE_BR0:
        return "br0";
    case RTSP_NET_TYPE_WLAN0:
        return "wlan0";
    default:
        return NULL;
    }
}

static int get_interface_ip(const char *interface_name,
                            char *ip,
                            size_t ip_capacity)
{
    struct ifreq request;
    int socket_fd;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    memset(&request, 0, sizeof(request));
    request.ifr_addr.sa_family = AF_INET;
    strncpy(request.ifr_name, interface_name, sizeof(request.ifr_name) - 1U);
    if (ioctl(socket_fd, SIOCGIFADDR, &request) < 0) {
        close(socket_fd);
        return -1;
    }

    close(socket_fd);
    const struct sockaddr_in *address =
        reinterpret_cast<const struct sockaddr_in *>(&request.ifr_addr);
    if (inet_ntop(AF_INET, &address->sin_addr, ip, ip_capacity) == NULL) {
        return -1;
    }
    return 0;
}

static int valid_id(int id)
{
    return id >= 0 && id < RTSP_SERVER_COUNT;
}

int rtsp_server_open(int id, const RtspServerConfig *config)
{
    const char *interface_name;
    char ip[INET_ADDRSTRLEN];
    char stream_name[16];
    MediaStream::MediaStreamAttr attributes;

    if (!valid_id(id) || config == NULL || config->frame_rate <= 0 ||
        g_servers[id] != NULL) {
        return -1;
    }

    interface_name = net_type_to_name(config->net_type);
    if (interface_name == NULL ||
        get_interface_ip(interface_name, ip, sizeof(ip)) != 0) {
        printf("[RTSP] Cannot get IP for interface %s: %s\n",
               interface_name != NULL ? interface_name : "unknown",
               strerror(errno));
        return -1;
    }

    g_servers[id] = TinyServer::createServer(std::string(ip), RTSP_SERVER_PORT);
    if (g_servers[id] == NULL) {
        printf("[RTSP] Create server failed on %s:%d\n", ip, RTSP_SERVER_PORT);
        return -1;
    }

    attributes.videoType = MediaStream::MediaStreamAttr::VIDEO_TYPE_H264;
    attributes.audioType = MediaStream::MediaStreamAttr::AUDIO_TYPE_AAC;
    attributes.streamType = MediaStream::MediaStreamAttr::STREAM_TYPE_UNICAST;
    snprintf(stream_name, sizeof(stream_name), "ch%d", id);
    g_streams[id] = g_servers[id]->createMediaStream(stream_name, attributes);
    if (g_streams[id] == NULL) {
        delete g_servers[id];
        g_servers[id] = NULL;
        return -1;
    }

    g_streams[id]->setVideoFrameRate(config->frame_rate);
    printf("============================================================\n");
    printf("[RTSP] URL: rtsp://%s:%d/%s\n", ip, RTSP_SERVER_PORT, stream_name);
    printf("============================================================\n");
    return 0;
}

int rtsp_server_start(int id)
{
    if (!valid_id(id) || g_servers[id] == NULL || g_streams[id] == NULL) {
        return -1;
    }
    return g_servers[id]->runWithNewThread();
}

int rtsp_server_send_video(int id,
                           unsigned char *data,
                           unsigned int size,
                           uint64_t pts,
                           RtspFrameType frame_type)
{
    MediaStream::FrameDataType media_frame_type;

    if (!valid_id(id) || g_streams[id] == NULL || data == NULL || size == 0U) {
        return -1;
    }

    media_frame_type = frame_type == RTSP_FRAME_TYPE_I
                           ? MediaStream::FRAME_DATA_TYPE_I
                           : MediaStream::FRAME_DATA_TYPE_P;
    g_streams[id]->appendVideoData(data, size, pts, media_frame_type);
    return 0;
}

void rtsp_server_stop(int id)
{
    if (valid_id(id) && g_servers[id] != NULL) {
        g_servers[id]->stop();
    }
}

void rtsp_server_close(int id)
{
    if (!valid_id(id)) {
        return;
    }
    delete g_streams[id];
    g_streams[id] = NULL;
    delete g_servers[id];
    g_servers[id] = NULL;
}
