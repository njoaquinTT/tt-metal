// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "eth_l1_address_map.h"
#include "noc/noc_parameters.h"

namespace tt::tt_fabric {

typedef struct _endpoint_sync {
    uint32_t sync_addr : 24;
    uint32_t endpoint_type : 8;
} endpoint_sync_t;

static_assert(sizeof(endpoint_sync_t) == 4);

constexpr uint32_t PACKET_WORD_SIZE_BYTES = 16;
constexpr uint32_t NUM_WR_CMD_BUFS = 4;
constexpr uint32_t DEFAULT_MAX_NOC_SEND_WORDS = (NOC_MAX_BURST_WORDS * NOC_WORD_BYTES) / PACKET_WORD_SIZE_BYTES;
constexpr uint32_t DEFAULT_MAX_ETH_SEND_WORDS = 2 * 1024;
constexpr uint32_t FVC_SYNC_THRESHOLD = 256;

#define ASYNC_WR (0x1 << 0)
#define ASYNC_WR_RESP (0x1 << 1)
#define ASYNC_RD (0x1 << 2)
#define ASYNC_RD_RESP (0x1 << 3)
#define DSOCKET_WR (0x1 << 4)
#define SSOCKET_WR (0x1 << 5)
#define ATOMIC_INC (0x1 << 6)
#define ATOMIC_READ_INC (0x1 << 7)
#define SOCKET_OPEN (0x1 << 8)
#define SOCKET_CLOSE (0x1 << 9)
#define SOCKET_CONNECT (0x1 << 10)

#define INVALID 0x0
#define MCAST_ACTIVE 0x1
#define MCAST_DATA 0x2
#define SYNC 0x4
#define FORWARD 0x8
#define INLINE_FORWARD 0x10
#define PACK_N_FORWARD 0x20
#define TERMINATE 0x40
#define NOP 0xFF

typedef struct _tt_routing {
    uint32_t packet_size_bytes;
    uint16_t dst_mesh_id;  // Remote mesh
    uint16_t dst_dev_id;   // Remote device
    uint16_t src_mesh_id;  // Source mesh
    uint16_t src_dev_id;   // Source device
    uint16_t ttl;
    uint8_t version;
    uint8_t flags;
} tt_routing;

static_assert(sizeof(tt_routing) == 16);

typedef struct _tt_session {
    uint32_t command;
    uint32_t target_offset_l;  // RDMA address
    uint32_t target_offset_h;
    uint32_t ack_offset_l;  // fabric client local address for session command acknowledgement.
                            // This is complete end-to-end acknowledgement of sessoin command completion at the remote
                            // device.
    uint32_t ack_offset_h;
} tt_session;

static_assert(sizeof(tt_session) == 20);

typedef struct _mcast_params {
    uint32_t socket_id;  // Socket Id for DSocket Multicast. Ignored for ASYNC multicast.
    uint16_t east;
    uint16_t west;
    uint16_t north;
    uint16_t south;
} mcast_params;

typedef struct _socket_params {
    uint32_t padding1;
    uint16_t socket_id;
    uint16_t epoch_id;
    uint8_t socket_type;
    uint8_t socket_direction;
    uint8_t routing_plane;
    uint8_t padding;
} socket_params;

typedef struct _atomic_params {
    uint32_t padding;
    uint32_t
        return_offset;  // L1 offset where atomic read should be returned. Noc X/Y is taken from tt_session.ack_offset
    uint32_t increment : 24;  // NOC atomic increment wrapping value.
    uint32_t wrap_boundary : 8;
} atomic_params;

typedef struct _async_wr_atomic_params {
    uint32_t padding;
    uint32_t l1_offset;
    uint32_t noc_xy : 24;
    uint32_t increment : 8;
} async_wr_atomic_params;

typedef struct _read_params {
    uint32_t return_offset_l;  // address where read data should be copied
    uint32_t return_offset_h;
    uint32_t size;  // number of bytes to read
} read_params;

typedef struct _misc_params {
    uint32_t words[3];
} misc_params;

typedef union _packet_params {
    mcast_params mcast_parameters;
    socket_params socket_parameters;
    atomic_params atomic_parameters;
    async_wr_atomic_params async_wr_atomic_parameters;
    read_params read_parameters;
    misc_params misc_parameters;
    uint8_t bytes[12];
} packet_params;

typedef struct _packet_header {
    packet_params packet_parameters;
    tt_session session;
    tt_routing routing;
} packet_header_t;

constexpr uint32_t PACKET_HEADER_SIZE_BYTES = 48;
constexpr uint32_t PACKET_HEADER_SIZE_WORDS = PACKET_HEADER_SIZE_BYTES / PACKET_WORD_SIZE_BYTES;

static_assert(sizeof(packet_header_t) == PACKET_HEADER_SIZE_BYTES);

static_assert(offsetof(packet_header_t, routing) % 4 == 0);

constexpr uint32_t packet_header_routing_offset_dwords = offsetof(packet_header_t, routing) / 4;

void tt_fabric_add_header_checksum(packet_header_t* p_header) {
    uint16_t* ptr = (uint16_t*)p_header;
    uint32_t sum = 0;
    for (uint32_t i = 2; i < sizeof(packet_header_t) / 2; i++) {
        sum += ptr[i];
    }
    sum = ~sum;
    sum += sum;
    p_header->packet_parameters.misc_parameters.words[0] = sum;
}

bool tt_fabric_is_header_valid(packet_header_t* p_header) {
#ifdef TT_FABRIC_DEBUG
    uint16_t* ptr = (uint16_t*)p_header;
    uint32_t sum = 0;
    for (uint32_t i = 2; i < sizeof(packet_header_t) / 2; i++) {
        sum += ptr[i];
    }
    sum = ~sum;
    sum += sum;
    return (p_header->packet_parameters.misc_parameters.words[0] == sum);
#else
    return true;
#endif
}

// This is a pull request entry for a fabric router.
// Pull request issuer populates these entries to identify
// the data that fabric router needs to pull from requestor.
// This data is the forwarded by router over ethernet.
// A pull request can be for packetized data or raw data, as specified by flags field.
//   - When registering a pull request for raw data, the requestor pushes two entries to router request queue.
//     First entry is packet_header, second entry is pull_request. This is typical of OP/Endpoint issuing read/writes
//     over tt-fabric.
//   - When registering a pull request for packetized data, the requetor only pushed pull_request entry to router
//   request queue.
//     This is typical of fabric routers forwarding data over noc/ethernet hops.
//
typedef struct _pull_request {
    uint32_t wr_ptr;        // Current value of write pointer.
    uint32_t rd_ptr;        // Current value of read pointer. Points to first byte of pull data.
    uint32_t size;          // Total number of bytes that need to be forwarded.
    uint32_t buffer_size;   // Producer local buffer size. Used for flow control when total data to send does not fit in
                            // local buffer.
    uint64_t buffer_start;  // Producer local buffer start. Used for wrapping rd/wr_ptr at the end of buffer.
    uint64_t ack_addr;  // Producer local address to send rd_ptr updates. fabric router pushes its rd_ptr to requestor
                        // at this address.
    uint32_t words_written;
    uint32_t words_read;
    uint8_t padding[7];
    uint8_t flags;  // Router command.
} pull_request_t;

constexpr uint32_t PULL_REQ_SIZE_BYTES = 48;

static_assert(sizeof(pull_request_t) == PULL_REQ_SIZE_BYTES);
static_assert(sizeof(pull_request_t) == sizeof(packet_header_t));

typedef union _chan_request_entry {
    pull_request_t pull_request;
    packet_header_t packet_header;
    uint8_t bytes[48];
} chan_request_entry_t;

constexpr uint32_t CHAN_PTR_SIZE_BYTES = 16;
typedef struct _chan_ptr {
    uint32_t ptr;
    uint32_t pad[3];
} chan_ptr;
static_assert(sizeof(chan_ptr) == CHAN_PTR_SIZE_BYTES);

constexpr uint32_t CHAN_REQ_BUF_LOG_SIZE = 4;  // must be 2^N
constexpr uint32_t CHAN_REQ_BUF_SIZE = 16;     // must be 2^N
constexpr uint32_t CHAN_REQ_BUF_SIZE_MASK = (CHAN_REQ_BUF_SIZE - 1);
constexpr uint32_t CHAN_REQ_BUF_PTR_MASK = ((CHAN_REQ_BUF_SIZE << 1) - 1);
constexpr uint32_t CHAN_REQ_BUF_SIZE_BYTES = 2 * CHAN_PTR_SIZE_BYTES + CHAN_REQ_BUF_SIZE * PULL_REQ_SIZE_BYTES;

typedef struct _chan_req_buf {
    chan_ptr wrptr;
    chan_ptr rdptr;
    chan_request_entry_t chan_req[CHAN_REQ_BUF_SIZE];
} chan_req_buf;

static_assert(sizeof(chan_req_buf) == CHAN_REQ_BUF_SIZE_BYTES);

typedef struct _local_pull_request {
    chan_ptr wrptr;
    chan_ptr rdptr;
    pull_request_t pull_request;
} local_pull_request_t;

typedef struct _chan_payload_ptr {
    uint32_t ptr;
    uint32_t pad[2];
    uint32_t ptr_cleared;
} chan_payload_ptr;

static_assert(sizeof(chan_payload_ptr) == CHAN_PTR_SIZE_BYTES);

// Fabric Virtual Control Channel (FVCC) parameters.
// Each control channel message is 48 Bytes.
// FVCC buffer is a 16 message buffer each for incoming and outgoing messages.
// Control message capacity can be increased by increasing FVCC_BUF_SIZE.
constexpr uint32_t FVCC_BUF_SIZE = 16;     // must be 2^N
constexpr uint32_t FVCC_BUF_LOG_SIZE = 4;  // must be log2(FVCC_BUF_SIZE)
constexpr uint32_t FVCC_SIZE_MASK = (FVCC_BUF_SIZE - 1);
constexpr uint32_t FVCC_PTR_MASK = ((FVCC_BUF_SIZE << 1) - 1);
constexpr uint32_t FVCC_BUF_SIZE_BYTES = PULL_REQ_SIZE_BYTES * FVCC_BUF_SIZE + 2 * CHAN_PTR_SIZE_BYTES;
constexpr uint32_t FVCC_SYNC_BUF_SIZE_BYTES = CHAN_PTR_SIZE_BYTES * FVCC_BUF_SIZE;

inline bool fvcc_buf_ptrs_empty(uint32_t wrptr, uint32_t rdptr) { return (wrptr == rdptr); }

inline bool fvcc_buf_ptrs_full(uint32_t wrptr, uint32_t rdptr) {
    uint32_t distance = wrptr >= rdptr ? wrptr - rdptr : wrptr + 2 * FVCC_BUF_SIZE - rdptr;
    return !fvcc_buf_ptrs_empty(wrptr, rdptr) && (distance >= FVCC_BUF_SIZE);
}

// out_req_buf has 16 byte additional storage per entry to hold the outgoing
// write pointer update. this is sent over ethernet.
// For incoming requests over ethernet, we only need storate for the request
// entry. The pointer update goes to fvcc state.
typedef struct _ctrl_chan_msg_buf {
    chan_ptr wrptr;
    chan_ptr rdptr;
    chan_request_entry_t msg_buf[FVCC_BUF_SIZE];
} ctrl_chan_msg_buf;

typedef struct _ctrl_chan_sync_buf {
    chan_payload_ptr ptr[FVCC_BUF_SIZE];
} ctrl_chan_sync_buf;

static_assert(sizeof(ctrl_chan_msg_buf) == FVCC_BUF_SIZE_BYTES);

typedef struct _sync_word {
    uint32_t val;
    uint32_t padding[3];
} sync_word_t;

typedef struct _gatekeeper_info {
    sync_word_t router_sync;
    sync_word_t ep_sync;
    uint32_t routing_planes;
    uint32_t padding[3];
    ctrl_chan_msg_buf gk_msg_buf;
} gatekeeper_info_t;

#define SOCKET_DIRECTION_SEND 1
#define SOCKET_DIRECTION_RECV 2
#define SOCKET_TYPE_DGRAM 1
#define SOCKET_TYPE_STREAM 2

enum SocketState : uint8_t {
    IDLE = 0,
    OPENING = 1,
    ACTIVE = 2,
    CLOSING = 3,
};

typedef struct _socket_handle {
    uint16_t socket_id;
    uint16_t epoch_id;
    uint8_t socket_state;
    uint8_t socket_type;
    uint8_t socket_direction;
    uint8_t rcvrs_ready;
    uint32_t routing_plane;
    uint16_t sender_mesh_id;
    uint16_t sender_dev_id;
    uint16_t rcvr_mesh_id;
    uint16_t rcvr_dev_id;
    uint32_t sender_handle;
    uint64_t pull_notification_adddr;
    uint64_t status_notification_addr;
    uint32_t padding[2];
} socket_handle_t;

static_assert(sizeof(socket_handle_t) % 16 == 0);

constexpr uint32_t MAX_SOCKETS = 64;
typedef struct _socket_info {
    uint32_t socket_count;
    uint32_t socket_setup_pending;
    uint32_t padding[2];
    socket_handle_t sockets[MAX_SOCKETS];
    chan_ptr wrptr;
    chan_ptr rdptr;
    chan_request_entry_t gk_message;
} socket_info_t;
static_assert(sizeof(socket_info_t) % 16 == 0);

typedef struct _fabric_client_interface {
    uint64_t gk_interface_addr;
    uint64_t gk_msg_buf_addr;
    uint64_t pull_req_buf_addr;
    uint32_t num_routing_planes;
    uint32_t routing_tables_l1_offset;
    uint32_t return_status[3];
    uint32_t socket_count;
    chan_ptr wrptr;
    chan_ptr rdptr;
    chan_request_entry_t gk_message;
    local_pull_request_t local_pull_request;
    socket_handle_t socket_handles[MAX_SOCKETS];
} fabric_client_interface_t;

static_assert(sizeof(fabric_client_interface_t) % 16 == 0);

constexpr uint32_t FABRIC_ROUTER_MISC_START = eth_l1_mem::address_map::ERISC_L1_UNRESERVED_BASE;
constexpr uint32_t FABRIC_ROUTER_MISC_SIZE = 256;
constexpr uint32_t FABRIC_ROUTER_SYNC_SEM = FABRIC_ROUTER_MISC_START;
constexpr uint32_t FABRIC_ROUTER_SYNC_SEM_SIZE = 16;

// Fabric Virtual Control Channel start/size
constexpr uint32_t FVCC_OUT_BUF_START = FABRIC_ROUTER_MISC_START + FABRIC_ROUTER_MISC_SIZE;
constexpr uint32_t FVCC_OUT_BUF_SIZE = FVCC_BUF_SIZE_BYTES;
constexpr uint32_t FVCC_SYNC_BUF_START = FVCC_OUT_BUF_START + FVCC_OUT_BUF_SIZE;
constexpr uint32_t FVCC_SYNC_BUF_SIZE = FVCC_SYNC_BUF_SIZE_BYTES;
constexpr uint32_t FVCC_IN_BUF_START = FVCC_SYNC_BUF_START + FVCC_SYNC_BUF_SIZE;
constexpr uint32_t FVCC_IN_BUF_SIZE = FVCC_BUF_SIZE_BYTES;

// Fabric Virtual Channel start/size
constexpr uint32_t FABRIC_ROUTER_REQ_QUEUE_START = FVCC_IN_BUF_START + FVCC_IN_BUF_SIZE;
constexpr uint32_t FABRIC_ROUTER_REQ_QUEUE_SIZE = sizeof(chan_req_buf);
constexpr uint32_t FABRIC_ROUTER_DATA_BUF_START = FABRIC_ROUTER_REQ_QUEUE_START + FABRIC_ROUTER_REQ_QUEUE_SIZE;

}  // namespace tt::tt_fabric
