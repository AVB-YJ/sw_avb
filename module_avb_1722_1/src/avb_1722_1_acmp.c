#include "avb_1722_common.h"
#include "avb_1722_1_common.h"
#include "avb_1722_1_acmp.h"
#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
#include "avb_1722_1_acmp_debug.h"
#include "simple_printf.h"
#endif
#include "avb_api.h"
#include "misc_timer.h"
#ifdef AVB_1722_1_ENABLE_ASSERTIONS
#include <assert.h>
#endif
#include <string.h>
#include <print.h>

#define STORE_RX_COMMAND

/* Inflight command defines */
#define CONTROLLER	0
#define LISTENER	1

#define TRUE 1
#define FALSE 0

enum {	ACMP_CONTROLLER_IDLE,
		ACMP_CONTROLLER_WAITING,
		ACMP_CONTROLLER_TIMEOUT,
		ACMP_CONTROLLER_CONNECT_RX_RESPONSE,
		ACMP_CONTROLLER_DISCONNECT_RX_RESPONSE,
		ACMP_CONTROLLER_GET_TX_STATE_RESPONSE,
		ACMP_CONTROLLER_GET_RX_STATE_RESPONSE,
		ACMP_CONTROLLER_GET_TX_CONNECTION_RESPONSE
} acmp_controller_state = ACMP_CONTROLLER_IDLE;

enum {	ACMP_TALKER_IDLE,
		ACMP_TALKER_WAITING,
		ACMP_TALKER_CONNECT,
		ACMP_TALKER_DISCONNECT,
		ACMP_TALKER_GET_STATE,
		ACMP_TALKER_GET_CONNECTION,
		ACMP_TALKER_WAITING_FOR_CONNECT,
		ACMP_TALKER_WAITING_FOR_DISCONNECT
} acmp_talker_state = ACMP_TALKER_IDLE;

enum {	ACMP_LISTENER_IDLE,
		ACMP_LISTENER_WAITING,
		ACMP_LISTENER_CONNECT_RX_COMMAND,
		ACMP_LISTENER_DISCONNECT_RX_COMMAND,
		ACMP_LISTENER_CONNECT_TX_RESPONSE,
		ACMP_LISTENER_DISCONNECT_TX_RESPONSE,
		ACMP_LISTENER_GET_STATE,
		ACMP_LISTENER_WAITING_FOR_CONNECT,
		ACMP_LISTENER_WAITING_FOR_DISCONNECT,
		ACMP_LISTENER_RX_TIMEOUT
} acmp_listener_state = ACMP_LISTENER_IDLE;

extern unsigned int avb_1722_1_buf[];
extern guid_t my_guid;

static const unsigned char avb_1722_1_acmp_dest_addr[6] = AVB_1722_1_ACMP_DEST_MAC;

// The channel counts defined by each bit in the ACMP default audio format word
static const unsigned char avb_1722_1_acmp_default_format_channel_counts[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24
};

// The sample rates defined by each bit in the ACMP default audio format word
static const unsigned int avb_1722_1_acmp_default_format_frequency[] = {
	44100, 48000, 88200, 96000, 176400, 192000
};

// ACMP command timeouts (in centiseconds) as defined in Table 7.4 of the specification
static const unsigned int avb_1722_1_acmp_inflight_timeouts[] = {20, 2, 2, 45, 5, 2, 2};

// Stream info lists
static avb_1722_1_acmp_listener_stream_info acmp_listener_streams[AVB_1722_1_MAX_LISTENERS];
static avb_1722_1_acmp_talker_stream_info acmp_talker_streams[AVB_1722_1_MAX_TALKERS];

// Inflight command lists
static avb_1722_1_acmp_inflight_command acmp_controller_inflight_commands[AVB_1722_1_MAX_INFLIGHT_COMMANDS];
static avb_1722_1_acmp_inflight_command acmp_listener_inflight_commands[AVB_1722_1_MAX_INFLIGHT_COMMANDS];

static unsigned acmp_centisecond_counter[2];
static int acmp_inflight_timeout_idx[2];
static avb_timer acmp_inflight_timer[2];

// Controller command
static avb_1722_1_acmp_cmd_resp acmp_controller_cmd;

// Talker's rcvdCmdResp
static avb_1722_1_acmp_cmd_resp acmp_talker_rcvd_cmd_resp;

// Listener's rcvdCmdResp
static avb_1722_1_acmp_cmd_resp acmp_listener_rcvd_cmd_resp;

static short sequence_id[2];

static void acmp_zero_listener_stream_info(int unique_id);
static void acmp_zero_talker_stream_info(int unique_id);

/**
 * Initialises ACMP state machines, data structures and timers.
 *
 * Must be called before any other ACMP function.
 */
void avb_1722_1_acmp_controller_init()
{
	acmp_controller_state = ACMP_CONTROLLER_WAITING;
	memset(acmp_controller_inflight_commands, 0, sizeof(avb_1722_1_acmp_inflight_command) * AVB_1722_1_MAX_INFLIGHT_COMMANDS);

	sequence_id[CONTROLLER] = 0;

	init_avb_timer(&acmp_inflight_timer[CONTROLLER], 10);
	acmp_centisecond_counter[CONTROLLER] = 0;
	start_avb_timer(&acmp_inflight_timer[CONTROLLER], 1);
}

void avb_1722_1_acmp_talker_init()
{
	int i;
	acmp_talker_state = ACMP_TALKER_WAITING;

	for (i = 0; i < AVB_1722_1_MAX_TALKERS; i++) acmp_zero_talker_stream_info(i);
}

void avb_1722_1_load(int offset,
                     char x[],
                     int size);

void avb_1722_1_acmp_listener_init()
{
  int i;
  char cmd;
	acmp_listener_state = ACMP_LISTENER_WAITING;
	memset(acmp_listener_inflight_commands, 0, sizeof(avb_1722_1_acmp_inflight_command) * AVB_1722_1_MAX_INFLIGHT_COMMANDS);

	for (i = 0; i < AVB_1722_1_MAX_LISTENERS; i++) acmp_zero_listener_stream_info(i);

	sequence_id[LISTENER] = 0;

        init_avb_timer(&acmp_inflight_timer[LISTENER], 10);
        acmp_centisecond_counter[LISTENER] = 0;
        start_avb_timer(&acmp_inflight_timer[LISTENER], 1);

#ifdef STORE_RX_COMMAND
        avb_1722_1_load(0,
                        &cmd,
                        1);

        //simple_printf("cmd:%d,%d",(int) cmd,ACMP_LISTENER_CONNECT_RX_COMMAND);
        if (cmd == ACMP_LISTENER_CONNECT_RX_COMMAND) {
          avb_1722_1_load(1,
                          (char *) &acmp_listener_rcvd_cmd_resp,
                          sizeof(avb_1722_1_acmp_packet_t));
          acmp_listener_state = ACMP_LISTENER_CONNECT_RX_COMMAND;
          //acmp_listener_rcvd_cmd_resp.controller_guid.l = 0;
        }
#endif
}

/**
 * Creates a valid ACMP Ethernet packet in avb_1722_1_buf[] from a supplied command structure
 */
static void avb_1722_1_create_acmp_packet(avb_1722_1_acmp_cmd_resp *cr, int message_type, int status)
{
	struct ethernet_hdr_t *hdr = (ethernet_hdr_t*) &avb_1722_1_buf[0];
	avb_1722_1_acmp_packet_t *pkt = (avb_1722_1_acmp_packet_t*) (hdr + AVB_1722_1_PACKET_BODY_POINTER_OFFSET);

	avb_1722_1_create_1722_1_header(avb_1722_1_acmp_dest_addr, DEFAULT_1722_1_ACMP_SUBTYPE, message_type, status, AVB_1722_1_ACMP_CD_LENGTH, hdr);

	SET_LONG_WORD(pkt->stream_id, cr->stream_id);
	SET_LONG_WORD(pkt->controller_guid, cr->controller_guid);
	SET_LONG_WORD(pkt->listener_guid, cr->listener_guid);
	SET_LONG_WORD(pkt->talker_guid, cr->talker_guid);
	HTON_U32(pkt->default_format, cr->default_format);
	HTON_U16(pkt->talker_unique_id, cr->talker_unique_id);
	HTON_U16(pkt->listener_unique_id, cr->listener_unique_id);
	HTON_U16(pkt->connection_count, cr->connection_count);
	HTON_U16(pkt->sequence_id, cr->sequence_id);
	HTON_U16(pkt->flags, cr->flags);
	pkt->dest_mac[0] = cr->stream_dest_mac[0];
	pkt->dest_mac[1] = cr->stream_dest_mac[1];
	pkt->dest_mac[2] = cr->stream_dest_mac[2];
	pkt->dest_mac[3] = cr->stream_dest_mac[3];
	pkt->dest_mac[4] = cr->stream_dest_mac[4];
	pkt->dest_mac[5] = cr->stream_dest_mac[5];
}

static void acmp_progress_inflight_timer(int entity_type)
{
	if (avb_timer_expired(&acmp_inflight_timer[entity_type]))
	{
		acmp_centisecond_counter[entity_type]++;
		start_avb_timer(&acmp_inflight_timer[entity_type], 1);
	}
}

/*
 * Returns a pointer to the correct inflight command list for a talker/listener/controller
 */
static avb_1722_1_acmp_inflight_command *acmp_get_inflight_list(int entity_type)
{
	avb_1722_1_acmp_inflight_command *inflight;

#ifdef AVB_1722_1_ENABLE_ASSERTIONS
	assert(entity_type == CONTROLLER || entity_type == LISTENER);
#endif

	switch (entity_type)
	{
		case CONTROLLER: inflight = &acmp_controller_inflight_commands[0]; break;
		case LISTENER: inflight = &acmp_listener_inflight_commands[0]; break;
	}

	return inflight;
}

/*
 * Returns the index into the inflight list for a specific received command's sequence id
 *
 * Returns -1 if the sequence id is not found in the list
 */
static int acmp_get_inflight_from_sequence_id(int entity_type, unsigned short id)
{
	int i;
	avb_1722_1_acmp_inflight_command *inflight = acmp_get_inflight_list(entity_type);

	for (i = 0; i < AVB_1722_1_MAX_INFLIGHT_COMMANDS; ++i)
	{
		if (inflight[i].in_use && (inflight[i].command.sequence_id == id))
		{
			return i;
		}
	}

	return -1;
}

/**
 * Sets the timeout field of an inflight command to the current time plus offset for
 * the particular message type
 */
static void acmp_update_inflight_timeout(int entity_type, avb_1722_1_acmp_inflight_command *inflight, unsigned int message_type)
{
	// Must check the message type is a valid value before doing the timeout array access
	if ((message_type % 2 == 0) && (message_type >= 0) && (message_type <= 12))
	{
		inflight->timeout = acmp_centisecond_counter[entity_type] + avb_1722_1_acmp_inflight_timeouts[message_type/2];
	}
	else
	{
		inflight->timeout = 0;
		// TODO: Define behaviour for invalid packets in production firmware
		printstrln("Error: Invalid message_type in acmp_update_inflight_timeout()");
	}
}

static void acmp_set_inflight_retry(int entity_type, unsigned int message_type, int inflight_idx)
{
	avb_1722_1_acmp_inflight_command *inflight = acmp_get_inflight_list(entity_type);

	acmp_update_inflight_timeout(entity_type, &inflight[inflight_idx], message_type);

	inflight[inflight_idx].retried = 1;
}

static void acmp_add_inflight(int entity_type, unsigned int message_type)
{
	int i;
	avb_1722_1_acmp_inflight_command *inflight = acmp_get_inflight_list(entity_type);

	for (i = 0; i < AVB_1722_1_MAX_INFLIGHT_COMMANDS; i++)
	{
		if (inflight[i].in_use) continue;

		inflight[i].in_use = 1;
		inflight[i].retried = 0;

		switch (entity_type)
		{
			case CONTROLLER: inflight[i].command = acmp_controller_cmd; break;
			case LISTENER: inflight[i].command = acmp_listener_rcvd_cmd_resp; break;
		}

		inflight[i].command.message_type = message_type;
		inflight[i].original_sequence_id = inflight[i].command.sequence_id;

		acmp_update_inflight_timeout(entity_type, &inflight[i], message_type);

		return;
	}

	// TODO: Return error if inflight command list is full
}

static avb_1722_1_acmp_inflight_command *acmp_remove_inflight(int entity_type)
{
	avb_1722_1_acmp_cmd_resp *acmp_command;
	avb_1722_1_acmp_inflight_command *inflight = acmp_get_inflight_list(entity_type);
	int index;

	switch (entity_type)
	{
		case CONTROLLER: acmp_command = &acmp_controller_cmd; break;
		case LISTENER: acmp_command = &acmp_listener_rcvd_cmd_resp; break;
	}

	index = acmp_get_inflight_from_sequence_id(entity_type, acmp_command->sequence_id);

	inflight[index].in_use = 0;

	return &inflight[index];
}

static int acmp_check_inflight_command_timeouts(int entity_type)
{
	int i;
	avb_1722_1_acmp_inflight_command *inflight = acmp_get_inflight_list(entity_type);

	for (i = 0; i < AVB_1722_1_MAX_INFLIGHT_COMMANDS; i++)
	{
		if (inflight[i].in_use)
		{
			if (acmp_centisecond_counter[entity_type] >= inflight[i].timeout) return i;
		}
	}

	return -1;
}


static void acmp_send_command(int entity_type, int message_type, avb_1722_1_acmp_cmd_resp *command, int retry, int inflight_idx, chanend c_tx)
{
	command->sequence_id = sequence_id[entity_type];
	sequence_id[entity_type]++;

	avb_1722_1_create_acmp_packet(command, message_type, ACMP_STATUS_SUCCESS);
	mac_tx(c_tx, avb_1722_1_buf, AVB_1722_1_ACMP_PACKET_SIZE, ETH_BROADCAST);

	if (!retry)
	{
		acmp_add_inflight(entity_type, message_type);
	}
	else
	{
#ifdef AVB_1722_1_ENABLE_ASSERTIONS
		assert(inflight_idx >= 0);
#endif
		acmp_set_inflight_retry(entity_type, message_type, inflight_idx);
	}
}

static void acmp_send_response(int message_type, avb_1722_1_acmp_cmd_resp *response, int status, chanend c_tx)
{
	avb_1722_1_create_acmp_packet(response, message_type, status);
	mac_tx(c_tx, avb_1722_1_buf, AVB_1722_1_ACMP_PACKET_SIZE, ETH_BROADCAST);
}

static void acmp_set_talker_response()
{
	int i;
	int talker = acmp_talker_rcvd_cmd_resp.talker_unique_id;

	acmp_talker_rcvd_cmd_resp.stream_id = acmp_talker_streams[talker].stream_id;
	for (i=0; i < 6; i++) acmp_talker_rcvd_cmd_resp.stream_dest_mac[i] = acmp_talker_streams[talker].destination_mac[i];

	acmp_talker_rcvd_cmd_resp.connection_count = acmp_talker_streams[talker].connection_count;
}

void acmp_controller_connect(guid_t *talker_guid, guid_t *listener_guid, chanend c_tx)
{
	acmp_controller_cmd.controller_guid = my_guid;
	acmp_controller_cmd.talker_guid.l = talker_guid->l;
	acmp_controller_cmd.listener_guid.l = listener_guid->l;

	// TODO: Default audio format

	acmp_send_command(CONTROLLER, ACMP_CMD_CONNECT_RX_COMMAND, &acmp_controller_cmd, FALSE, -1, c_tx);
}

void acmp_controller_disconnect(guid_t *talker_guid, guid_t *listener_guid, chanend c_tx)
{
	acmp_controller_cmd.controller_guid = my_guid;
	acmp_controller_cmd.talker_guid.l = talker_guid->l;
	acmp_controller_cmd.listener_guid.l = listener_guid->l;

	acmp_send_command(CONTROLLER, ACMP_CMD_DISCONNECT_RX_COMMAND, &acmp_controller_cmd, FALSE, -1, c_tx);
}

static void store_rcvd_cmd_resp(avb_1722_1_acmp_cmd_resp* store, avb_1722_1_acmp_packet_t* pkt)
{
	GET_LONG_WORD(store->stream_id, pkt->stream_id);
	GET_LONG_WORD(store->controller_guid, pkt->controller_guid);
	GET_LONG_WORD(store->listener_guid, pkt->listener_guid);
	GET_LONG_WORD(store->talker_guid, pkt->talker_guid);
	GET_WORD(store->default_format, pkt->default_format);
	store->talker_unique_id = NTOH_U16(pkt->talker_unique_id);
	store->listener_unique_id = NTOH_U16(pkt->listener_unique_id);
	store->connection_count = NTOH_U16(pkt->connection_count);
	store->sequence_id = NTOH_U16(pkt->sequence_id);
	store->flags = NTOH_U16(pkt->flags);
	store->stream_dest_mac[0] = pkt->dest_mac[0];
	store->stream_dest_mac[1] = pkt->dest_mac[1];
	store->stream_dest_mac[2] = pkt->dest_mac[2];
	store->stream_dest_mac[3] = pkt->dest_mac[3];
	store->stream_dest_mac[4] = pkt->dest_mac[4];
	store->stream_dest_mac[5] = pkt->dest_mac[5];
	store->message_type = GET_1722_1_MSG_TYPE(&(pkt->header));
	store->status = GET_1722_1_VALID_TIME(&(pkt->header));
}

/**
 * Returns 1 if the listener unique id in a received command is within a valid range.
 *
 * Else returns 0.
 */
static unsigned acmp_listener_valid_listener_unique()
{
	return acmp_listener_rcvd_cmd_resp.listener_unique_id < AVB_1722_1_MAX_LISTENERS;
}

/**
 * Returns 1 if the listener with unique id in a received command is currently connected.
 *
 * Else returns 0.
 */
static unsigned acmp_listener_is_connected()
{
	enum avb_sink_state_t state;
	get_avb_sink_state(acmp_listener_rcvd_cmd_resp.listener_unique_id, &state);
	return state != AVB_SINK_STATE_DISABLED;
}

/**
 * Sets all fields in the listener stream info entry with unique_id to zero
 */
static void acmp_zero_listener_stream_info(int unique_id)
{
	memset(&acmp_listener_streams[unique_id], 0, sizeof(avb_1722_1_acmp_listener_stream_info));
}

static void acmp_add_listener_stream_info()
{
	int i;
	int unique_id = acmp_listener_rcvd_cmd_resp.listener_unique_id;

	acmp_listener_streams[unique_id].connected = 1;
	for (i = 0; i < 6; i++) acmp_listener_streams[unique_id].destination_mac[i] = acmp_listener_rcvd_cmd_resp.stream_dest_mac[i];
	acmp_listener_streams[unique_id].stream_id = acmp_listener_rcvd_cmd_resp.stream_id;
	acmp_listener_streams[unique_id].talker_guid = acmp_listener_rcvd_cmd_resp.talker_guid;
	acmp_listener_streams[unique_id].talker_unique_id = acmp_listener_rcvd_cmd_resp.talker_unique_id;
}

static avb_1722_1_acmp_status_t acmp_listener_get_state()
{
	int i;
	int unique_id = acmp_listener_rcvd_cmd_resp.listener_unique_id;

	acmp_listener_rcvd_cmd_resp.stream_id = acmp_listener_streams[unique_id].stream_id;
	for (i = 0; i < 6; i++) acmp_listener_rcvd_cmd_resp.stream_dest_mac[i] = acmp_listener_streams[unique_id].destination_mac[i];
	acmp_listener_rcvd_cmd_resp.talker_guid = acmp_listener_streams[unique_id].talker_guid;
	acmp_listener_rcvd_cmd_resp.talker_unique_id = acmp_listener_streams[unique_id].talker_unique_id;

	/* If for some reason we couldn't get the state, we could return a STATE_UNVAILABLE status instead */

	return ACMP_STATUS_SUCCESS;
}

static avb_1722_1_acmp_status_t acmp_talker_get_state()
{
	int i;
	int unique_id = acmp_talker_rcvd_cmd_resp.talker_unique_id;

	acmp_talker_rcvd_cmd_resp.stream_id = acmp_talker_streams[unique_id].stream_id;
	for (i = 0; i < 6; i++) acmp_talker_rcvd_cmd_resp.stream_dest_mac[i] = acmp_talker_streams[unique_id].destination_mac[i];
	acmp_talker_rcvd_cmd_resp.connection_count = acmp_talker_streams[unique_id].connection_count;

	return ACMP_STATUS_SUCCESS;
}

static avb_1722_1_acmp_status_t acmp_talker_get_connection()
{
	int i;
	int unique_id = acmp_talker_rcvd_cmd_resp.talker_unique_id;
	unsigned short connection = acmp_talker_rcvd_cmd_resp.connection_count;

	// Check if connection exists
	if (connection >= AVB_1722_1_MAX_LISTENERS_PER_TALKER || acmp_talker_streams[unique_id].connected_listeners[connection].guid.l == 0)
	{
		return ACMP_STATUS_NO_SUCH_CONNECTION;
	}

	acmp_talker_rcvd_cmd_resp.stream_id = acmp_talker_streams[unique_id].stream_id;
	for (i = 0; i < 6; i++) acmp_talker_rcvd_cmd_resp.stream_dest_mac[i] = acmp_talker_streams[unique_id].destination_mac[i];

	acmp_talker_rcvd_cmd_resp.listener_guid.l = acmp_talker_streams[unique_id].connected_listeners[connection].guid.l;
	acmp_talker_rcvd_cmd_resp.listener_unique_id = acmp_talker_streams[unique_id].connected_listeners[connection].unique_id;

	return ACMP_STATUS_SUCCESS;
}

static void acmp_add_talker_stream_info()
{
	int i;
	int unique_id = acmp_talker_rcvd_cmd_resp.talker_unique_id;

	// Check listener_guid and listener_unique_id are not in connected_listeners as per 7.2.2.6.2.2. of the spec
	for (i = 0; i < AVB_1722_1_MAX_LISTENERS_PER_TALKER; i++)
	{
		if (acmp_talker_streams[unique_id].connected_listeners[i].guid.l == acmp_talker_rcvd_cmd_resp.listener_guid.l &&
				acmp_talker_streams[unique_id].connected_listeners[i].unique_id == acmp_talker_rcvd_cmd_resp.listener_unique_id)
		{
			// Listener already connected
			return;
		}

		if (acmp_talker_streams[unique_id].connected_listeners[i].guid.l == 0) // The first empty connected_listeners field
		{
			acmp_talker_streams[unique_id].connected_listeners[i].guid.l = acmp_talker_rcvd_cmd_resp.listener_guid.l;
			acmp_talker_streams[unique_id].connected_listeners[i].unique_id = acmp_talker_rcvd_cmd_resp.listener_unique_id;
			acmp_talker_streams[unique_id].connection_count++;
			break;
		}
	}
}

static void acmp_remove_talker_stream_info()
{
	int i;
	int unique_id = acmp_talker_rcvd_cmd_resp.talker_unique_id;

	for (i = 0; i < AVB_1722_1_MAX_LISTENERS_PER_TALKER; i++)
	{
		if (acmp_talker_streams[unique_id].connected_listeners[i].guid.l == acmp_talker_rcvd_cmd_resp.listener_guid.l &&
				acmp_talker_streams[unique_id].connected_listeners[i].unique_id == acmp_talker_rcvd_cmd_resp.listener_unique_id)
		{
			acmp_talker_streams[unique_id].connected_listeners[i].guid.l = 0;
			acmp_talker_streams[unique_id].connected_listeners[i].unique_id = 0;
			acmp_talker_streams[unique_id].connection_count--;

#ifdef AVB_1722_1_ENABLE_ASSERTIONS
			assert(acmp_talker_streams[unique_id].connection_count >= 0);
#endif

			/* TODO: 7.2.2.6.2.4: "If the connection_count is now equal to zero (0) there are no more listeners for
			 * the stream and the talker may deallocate the stream index and destination multicast MAC and it shall
			 * initiate an SRP talker de-registration.
			 */

			return;
		}
	}
}

/**
 * Sets all fields in the talker stream info entry with unique_id to zero
 */
static void acmp_zero_talker_stream_info(int unique_id)
{
#ifdef AVB_1722_1_ENABLE_ASSERTIONS
	assert(unique_id < AVB_1722_1_MAX_TALKERS);
#endif
	memset(&acmp_talker_streams[unique_id], 0, sizeof(avb_1722_1_acmp_talker_stream_info));
}

/**
 * Returns 1 if the talker unique id in a received command is within a valid range.
 *
 * Else returns 0.
 */
static unsigned acmp_talker_valid_talker_unique()
{
	return acmp_talker_rcvd_cmd_resp.listener_unique_id < AVB_1722_1_MAX_TALKERS;
}

static void process_avb_1722_1_acmp_controller_packet(avb_status_t *status, unsigned char message_type, avb_1722_1_acmp_packet_t* pkt)
{
	int inflight_index = 0;

	status->info.a1722_1.msg = AVB_1722_1_OK;

	if (acmp_controller_state != ACMP_CONTROLLER_WAITING) return;
	if (compare_guid(pkt->controller_guid, &my_guid) == 0) return;

	inflight_index = acmp_get_inflight_from_sequence_id(CONTROLLER, NTOH_U16(pkt->sequence_id));
	if (inflight_index < 0) return;	// We don't have an inflight entry for this command

	if (message_type != (acmp_controller_inflight_commands[inflight_index].command.message_type + 1)) return;

	store_rcvd_cmd_resp(&acmp_controller_cmd, pkt);

	switch (message_type)
	{
		case ACMP_CMD_CONNECT_RX_RESPONSE:
			acmp_controller_state = ACMP_CONTROLLER_CONNECT_RX_RESPONSE;
			break;
		case ACMP_CMD_DISCONNECT_RX_RESPONSE:
			acmp_controller_state = ACMP_CONTROLLER_DISCONNECT_RX_RESPONSE;
			break;
		case ACMP_CMD_GET_TX_STATE_RESPONSE:
			acmp_controller_state = ACMP_CONTROLLER_GET_RX_STATE_RESPONSE;
			break;
		case ACMP_CMD_GET_RX_STATE_RESPONSE:
			acmp_controller_state = ACMP_CONTROLLER_GET_RX_STATE_RESPONSE;
			break;
		case ACMP_CMD_GET_TX_CONNECTION_RESPONSE:
			acmp_controller_state = ACMP_CONTROLLER_GET_TX_CONNECTION_RESPONSE;
			break;
		default:
		{
			acmp_controller_state = ACMP_CONTROLLER_WAITING;
			break;
		}
	}

	return;
}

static void process_avb_1722_1_acmp_talker_packet(avb_status_t *status, unsigned char message_type, avb_1722_1_acmp_packet_t* pkt)
{
	status->info.a1722_1.msg = AVB_1722_1_OK;

	if (compare_guid(pkt->talker_guid, &my_guid) == 0) return;
	if (acmp_talker_state != ACMP_TALKER_WAITING) return;

	store_rcvd_cmd_resp(&acmp_talker_rcvd_cmd_resp, pkt);

	switch (message_type)
	{
	case ACMP_CMD_CONNECT_TX_COMMAND:
		acmp_talker_state = ACMP_TALKER_CONNECT;
		break;
	case ACMP_CMD_DISCONNECT_TX_COMMAND:
		acmp_talker_state = ACMP_TALKER_DISCONNECT;
		break;
	case ACMP_CMD_GET_TX_STATE_COMMAND:
		acmp_talker_state = ACMP_TALKER_GET_STATE;
		break;
	case ACMP_CMD_GET_TX_CONNECTION_COMMAND:
		acmp_talker_state = ACMP_TALKER_GET_CONNECTION;
		break;
	}

	return;
}

void avb_1722_1_store(int offset,
                      const char x[],
                      int size);

static void process_avb_1722_1_acmp_listener_packet(avb_status_t *status, unsigned char message_type, avb_1722_1_acmp_packet_t* pkt)
{
	status->info.a1722_1.msg = AVB_1722_1_OK;

	if (compare_guid(pkt->listener_guid, &my_guid)==0) return;
	if (acmp_listener_state != ACMP_LISTENER_WAITING) return;

	store_rcvd_cmd_resp(&acmp_listener_rcvd_cmd_resp, pkt);

	switch (message_type)
	{
	case ACMP_CMD_CONNECT_TX_RESPONSE:
		acmp_listener_state = ACMP_LISTENER_CONNECT_TX_RESPONSE;
		break;
	case ACMP_CMD_DISCONNECT_TX_RESPONSE:
		acmp_listener_state = ACMP_LISTENER_DISCONNECT_TX_RESPONSE;
		break;
	case ACMP_CMD_CONNECT_RX_COMMAND:
		acmp_listener_state = ACMP_LISTENER_CONNECT_RX_COMMAND;
#ifdef STORE_RX_COMMAND
                {
                  char x = ACMP_LISTENER_CONNECT_RX_COMMAND;
                  //simple_printf("cmd:%d,%d",(int) x,ACMP_LISTENER_CONNECT_RX_COMMAND);
                  avb_1722_1_store(0,
                                   &x,
                                   1);
                  avb_1722_1_store(1,
                                   (char *) &acmp_listener_rcvd_cmd_resp,
                                   sizeof(avb_1722_1_acmp_packet_t));
                }
#endif
		break;
	case ACMP_CMD_DISCONNECT_RX_COMMAND:
		acmp_listener_state = ACMP_LISTENER_DISCONNECT_RX_COMMAND;
#ifdef STORE_RX_COMMAND
                {
                  char x = 0;
                  avb_1722_1_store(0,
                                   &x,
                                   1);
                }
#endif
		break;
	case ACMP_CMD_GET_RX_STATE_COMMAND:
		acmp_listener_state = ACMP_LISTENER_GET_STATE;
		break;
	}

	return;
}

void process_avb_1722_1_acmp_packet(avb_status_t *status, avb_1722_1_acmp_packet_t* pkt, chanend c_tx)
{
	unsigned char message_type = GET_1722_1_MSG_TYPE(((avb_1722_1_packet_header_t*)pkt));

	status->info.a1722_1.msg = AVB_1722_1_OK;

	switch (message_type)
	{
		// Talker messages
		case ACMP_CMD_CONNECT_TX_COMMAND:
		case ACMP_CMD_DISCONNECT_TX_COMMAND:
		case ACMP_CMD_GET_TX_STATE_COMMAND:
		case ACMP_CMD_GET_TX_CONNECTION_COMMAND:
		{
			process_avb_1722_1_acmp_talker_packet(status, message_type, pkt);
			return;
		}

		// Listener messages
		case ACMP_CMD_CONNECT_TX_RESPONSE:
		case ACMP_CMD_DISCONNECT_TX_RESPONSE:
		case ACMP_CMD_CONNECT_RX_COMMAND:
		case ACMP_CMD_DISCONNECT_RX_COMMAND:
		case ACMP_CMD_GET_RX_STATE_COMMAND:
		{
			process_avb_1722_1_acmp_listener_packet(status, message_type, pkt);
			return;
		}

		// Controller messages
		case ACMP_CMD_CONNECT_RX_RESPONSE:
		case ACMP_CMD_DISCONNECT_RX_RESPONSE:
		case ACMP_CMD_GET_RX_STATE_RESPONSE:
		case ACMP_CMD_GET_TX_STATE_RESPONSE:
		case ACMP_CMD_GET_TX_CONNECTION_RESPONSE:
		{
			process_avb_1722_1_acmp_controller_packet(status, message_type, pkt);
			return;
		}
	}

	return;
}

void avb_1722_1_acmp_controller_periodic(avb_status_t *status, chanend c_tx)
{
	status->type = AVB_NO_STATUS;

	switch (acmp_controller_state)
	{
		case ACMP_CONTROLLER_IDLE:
		case ACMP_CONTROLLER_WAITING:
		{
			acmp_progress_inflight_timer(CONTROLLER);

			// acmp_inflight_timeout_idx is a global index provided to ACMP_CONTROLLER_TIMEOUT
			acmp_inflight_timeout_idx[CONTROLLER] = acmp_check_inflight_command_timeouts(CONTROLLER);

			if (acmp_inflight_timeout_idx[CONTROLLER] >= 0)	// An inflight command has timed out
			{
				acmp_controller_state = ACMP_CONTROLLER_TIMEOUT;
			}

			break;
		}
		case ACMP_CONTROLLER_TIMEOUT:
		{
			int i = acmp_inflight_timeout_idx[CONTROLLER];
			if (acmp_controller_inflight_commands[i].retried)
			{
				// Remove inflight command
				acmp_controller_inflight_commands[i].in_use = 0;

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
				simple_printf("ACMP Controller: Removed inflight %s with timed out retry - seq id: %d\n",
						debug_acmp_message_s[acmp_controller_inflight_commands[i].command.message_type],
						acmp_controller_inflight_commands[i].original_sequence_id);
#endif
			}
			else
			{
				acmp_send_command(CONTROLLER, acmp_controller_inflight_commands[i].command.message_type,
									&acmp_controller_inflight_commands[i].command, TRUE, i, c_tx);

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
				simple_printf("ACMP Controller: Sent retry for timed out %s - seq id: %d\n",
						debug_acmp_message_s[acmp_controller_inflight_commands[i].command.message_type],
						acmp_controller_inflight_commands[i].original_sequence_id);
#endif
			}

			acmp_controller_state = ACMP_CONTROLLER_WAITING;

			break;
		}
		// TODO:
		case ACMP_CONTROLLER_CONNECT_RX_RESPONSE:
		case ACMP_CONTROLLER_DISCONNECT_RX_RESPONSE:
		case ACMP_CONTROLLER_GET_TX_STATE_RESPONSE:
		case ACMP_CONTROLLER_GET_RX_STATE_RESPONSE:
		case ACMP_CONTROLLER_GET_TX_CONNECTION_RESPONSE:
		{
			// Remove inflight command
			avb_1722_1_acmp_inflight_command *inflight = acmp_remove_inflight(CONTROLLER);

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
			simple_printf("ACMP Controller: Removed inflight %s with response %s - seq id: %d\n",
					debug_acmp_message_s[inflight->command.message_type],
					debug_acmp_status_s[inflight->command.status],
					inflight->original_sequence_id);
#endif

			acmp_controller_state = ACMP_CONTROLLER_WAITING;

			break;
		}
	}

	return;
}

void avb_1722_1_acmp_talker_periodic(avb_status_t *status, chanend c_tx)
{
	status->type = AVB_NO_STATUS;

	switch (acmp_talker_state)
	{
		case ACMP_TALKER_IDLE:
		case ACMP_TALKER_WAITING:
		case ACMP_TALKER_WAITING_FOR_CONNECT:
		case ACMP_TALKER_WAITING_FOR_DISCONNECT:
		{
			return;
		}
		case ACMP_TALKER_CONNECT:
		{
			if (!acmp_talker_valid_talker_unique())
			{
				acmp_send_response(ACMP_CMD_CONNECT_TX_RESPONSE, &acmp_talker_rcvd_cmd_resp, ACMP_STATUS_TALKER_UNKNOWN_ID, c_tx);
				acmp_talker_state = ACMP_TALKER_WAITING;
				return;
			}
			else
			{
				acmp_talker_state = ACMP_TALKER_WAITING_FOR_CONNECT;
				status->type = AVB_1722_1;
				status->info.a1722_1.msg = AVB_1722_1_CONNECT_TALKER;
				return;
			}
			break;
		}
		case ACMP_TALKER_DISCONNECT:
		{
			if (!acmp_talker_valid_talker_unique())
			{
				acmp_send_response(ACMP_CMD_DISCONNECT_TX_RESPONSE, &acmp_talker_rcvd_cmd_resp, ACMP_STATUS_TALKER_UNKNOWN_ID, c_tx);
				acmp_talker_state = ACMP_TALKER_WAITING;
				return;
			}
			else
			{
				acmp_talker_state = ACMP_TALKER_WAITING_FOR_DISCONNECT;
				status->type = AVB_1722_1;
				status->info.a1722_1.msg = AVB_1722_1_DISCONNECT_TALKER;
				return;
			}
			break;
		}
		case ACMP_TALKER_GET_STATE:
		{
			int acmp_status;
			if (!acmp_talker_valid_talker_unique())
			{
				acmp_status = ACMP_STATUS_TALKER_UNKNOWN_ID;
			}
			else
			{
				acmp_status = acmp_talker_get_state();
			}
			acmp_send_response(ACMP_CMD_GET_TX_STATE_RESPONSE, &acmp_talker_rcvd_cmd_resp, acmp_status, c_tx);

			acmp_talker_state = ACMP_TALKER_WAITING;
			return;
		}
		case ACMP_TALKER_GET_CONNECTION:
		{
			int acmp_status;
			if (!acmp_talker_valid_talker_unique())
			{
				acmp_status = ACMP_STATUS_TALKER_UNKNOWN_ID;
			}
			else
			{
				acmp_status = acmp_talker_get_connection();
			}
			acmp_send_response(ACMP_CMD_GET_TX_CONNECTION_RESPONSE, &acmp_talker_rcvd_cmd_resp, acmp_status, c_tx);

			acmp_talker_state = ACMP_TALKER_WAITING;
			return;
		}
	}

	return;
}

void avb_1722_1_acmp_listener_periodic(avb_status_t *status, chanend c_tx)
{
	status->type = AVB_NO_STATUS;

	switch (acmp_listener_state)
	{
		case ACMP_LISTENER_IDLE:
		case ACMP_LISTENER_WAITING_FOR_CONNECT:
		case ACMP_LISTENER_WAITING_FOR_DISCONNECT:
			break;
		case ACMP_LISTENER_WAITING:
		{
			acmp_progress_inflight_timer(LISTENER);

			acmp_inflight_timeout_idx[LISTENER] = acmp_check_inflight_command_timeouts(LISTENER);

			if (acmp_inflight_timeout_idx[LISTENER] >= 0)	// An inflight command has timed out
			{
				acmp_listener_state = ACMP_LISTENER_RX_TIMEOUT;
			}
			break;
		}

		case ACMP_LISTENER_CONNECT_RX_COMMAND:
		{
			if (!acmp_listener_valid_listener_unique())
			{
				acmp_send_response(ACMP_CMD_CONNECT_RX_RESPONSE, &acmp_listener_rcvd_cmd_resp, ACMP_STATUS_LISTENER_UNKNOWN_ID, c_tx);
			}
			else
			{
				if (acmp_listener_is_connected())
				{
					acmp_send_response(ACMP_CMD_CONNECT_RX_RESPONSE, &acmp_listener_rcvd_cmd_resp, ACMP_STATUS_LISTENER_EXCLUSIVE, c_tx);
				}
				else
				{
					acmp_send_command(LISTENER, ACMP_CMD_CONNECT_TX_COMMAND, &acmp_listener_rcvd_cmd_resp, FALSE, -1, c_tx);
				}
			}
			acmp_listener_state = ACMP_LISTENER_WAITING;
			break;
		}
		case ACMP_LISTENER_DISCONNECT_RX_COMMAND:
		{
			if (!acmp_listener_valid_listener_unique())
			{
				acmp_send_response(ACMP_CMD_DISCONNECT_RX_RESPONSE, &acmp_listener_rcvd_cmd_resp, ACMP_STATUS_LISTENER_UNKNOWN_ID, c_tx);
			}
			else
			{
				if (acmp_listener_is_connected())
				{
					acmp_send_command(LISTENER, ACMP_CMD_DISCONNECT_TX_COMMAND, &acmp_listener_rcvd_cmd_resp, FALSE, -1, c_tx);;
				}
				else
				{
					acmp_send_response(ACMP_CMD_DISCONNECT_RX_RESPONSE, &acmp_listener_rcvd_cmd_resp, ACMP_STATUS_NOT_CONNECTED, c_tx);
				}
			}
			acmp_listener_state = ACMP_LISTENER_WAITING;
			break;
		}
		case ACMP_LISTENER_CONNECT_TX_RESPONSE:
		{
			if (acmp_listener_valid_listener_unique())
			{
				avb_1722_1_acmp_inflight_command *inflight;
				inflight = acmp_remove_inflight(LISTENER);
				acmp_listener_rcvd_cmd_resp.sequence_id = inflight->original_sequence_id; // FIXME: This is a bit messy

                                if (acmp_listener_rcvd_cmd_resp.controller_guid.l != 0) {
                                    acmp_send_response(ACMP_CMD_CONNECT_RX_RESPONSE, &acmp_listener_rcvd_cmd_resp, acmp_listener_rcvd_cmd_resp.status, c_tx);
                                }

				acmp_add_listener_stream_info();

				acmp_listener_state = ACMP_LISTENER_WAITING_FOR_CONNECT;

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
				simple_printf("ACMP Listener: Removed inflight CONNECT_TX_COMMAND with response %s - seq id: %d\n",
						debug_acmp_status_s[inflight->command.status],
						inflight->command.sequence_id);
#endif
				status->type = AVB_1722_1;
				status->info.a1722_1.msg = AVB_1722_1_CONNECT_LISTENER;

				return;
			}
			break;
		}
		case ACMP_LISTENER_DISCONNECT_TX_RESPONSE:
		{
			if (acmp_listener_valid_listener_unique())
			{
				avb_1722_1_acmp_inflight_command *inflight;
				inflight = acmp_remove_inflight(LISTENER);
				acmp_listener_rcvd_cmd_resp.sequence_id = inflight->original_sequence_id;

				acmp_send_response(ACMP_CMD_DISCONNECT_RX_RESPONSE, &acmp_listener_rcvd_cmd_resp, acmp_listener_rcvd_cmd_resp.status, c_tx);
				acmp_zero_listener_stream_info(acmp_listener_rcvd_cmd_resp.listener_unique_id);

				acmp_listener_state = ACMP_LISTENER_WAITING_FOR_DISCONNECT;

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
				simple_printf("ACMP Listener: Removed inflight DISCONNECT_TX_COMMAND with response %s - seq id: %d\n",
						debug_acmp_status_s[inflight->command.status],
						inflight->command.sequence_id);
#endif
				status->type = AVB_1722_1;
				status->info.a1722_1.msg = AVB_1722_1_DISCONNECT_LISTENER;

				return;
			}
			break;
		}
		case ACMP_LISTENER_GET_STATE:
		{
			int status;

			if (!acmp_listener_valid_listener_unique())
			{
				status = ACMP_STATUS_LISTENER_UNKNOWN_ID;
			}
			else
			{
				/* Sets appropriate state fields in acmp_listener_rcvd_cmd_resp for responding and returns status message */
				status = acmp_listener_get_state();
			}

			acmp_send_response(ACMP_CMD_GET_RX_STATE_RESPONSE, &acmp_listener_rcvd_cmd_resp, status, c_tx);

			acmp_listener_state = ACMP_LISTENER_WAITING;
			break;
		}
		case ACMP_LISTENER_RX_TIMEOUT:
		{
			int i = acmp_inflight_timeout_idx[LISTENER];
			avb_1722_1_acmp_inflight_command *inflight = &acmp_listener_inflight_commands[i];
			if (inflight->retried)
			{
				inflight->command.sequence_id = inflight->original_sequence_id;
				acmp_send_response(inflight->command.message_type + 1, &inflight->command, ACMP_STATUS_LISTENER_TALKER_TIMEOUT, c_tx);
				// Remove inflight command
				inflight->in_use = 0;

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
				simple_printf("ACMP Listener: Removed inflight %s with timed out retry - seq id: %d\n",
						debug_acmp_message_s[inflight->command.message_type],
						inflight->command.sequence_id);
#endif
			}
			else
			{
				int message_type = inflight->command.message_type;

				acmp_send_command(LISTENER, message_type, &inflight->command, TRUE, i, c_tx);

#ifdef AVB_1722_1_ACMP_DEBUG_INFLIGHT
				simple_printf("ACMP Listener:  Sent retry for timed out %s - seq id: %d\n",
						debug_acmp_message_s[inflight->command.message_type],
						inflight->command.sequence_id);
#endif
			}

			acmp_listener_state = ACMP_LISTENER_WAITING;
			break;
		}

	}

	return;
}

unsigned avb_1722_1_acmp_get_talker_connection_info(short *talker)
{
	*talker = acmp_talker_rcvd_cmd_resp.talker_unique_id;
	return 1;
}

unsigned avb_1722_1_acmp_get_listener_connection_info(short *listener, unsigned char address[6], unsigned streamId[2], unsigned* vlan)
{
	*listener = acmp_listener_rcvd_cmd_resp.listener_unique_id;
	for (unsigned c=0; c<6; ++c) address[c] = acmp_listener_rcvd_cmd_resp.stream_dest_mac[c];
	streamId[1] = (unsigned)(acmp_listener_rcvd_cmd_resp.stream_id.l >> 0);
	streamId[0] = (unsigned)(acmp_listener_rcvd_cmd_resp.stream_id.l >> 32);
	*vlan = 2;
	return 1;
}

void avb_1722_1_acmp_talker_connection_complete(short status, chanend c_tx)
{
	switch (acmp_talker_state)
	{
		case ACMP_TALKER_WAITING_FOR_CONNECT:
			acmp_add_talker_stream_info();
			acmp_set_talker_response();
			acmp_send_response(ACMP_CMD_CONNECT_TX_RESPONSE, &acmp_talker_rcvd_cmd_resp, status, c_tx);
			acmp_talker_state = ACMP_TALKER_WAITING;
			break;
		case ACMP_TALKER_WAITING_FOR_DISCONNECT:
			acmp_remove_talker_stream_info();
			acmp_set_talker_response();
			acmp_send_response(ACMP_CMD_DISCONNECT_TX_RESPONSE, &acmp_talker_rcvd_cmd_resp, status, c_tx);
			acmp_talker_state = ACMP_TALKER_WAITING;
			break;
		default:
			break;
	}
}


void avb_1722_1_acmp_listener_connection_complete(short status, chanend c_tx)
{
	switch (acmp_listener_state)
	{
		case ACMP_LISTENER_WAITING_FOR_CONNECT:
			acmp_listener_rcvd_cmd_resp.status = status;
			acmp_listener_state = ACMP_LISTENER_WAITING;
			break;
		case ACMP_LISTENER_WAITING_FOR_DISCONNECT:
			acmp_listener_rcvd_cmd_resp.status = status;
			acmp_listener_state = ACMP_LISTENER_WAITING;
			break;
		default:
			break;
	}
}

void avb_1722_1_talker_set_mac_address(unsigned talker_unique_id, unsigned char macaddr[])
{
	if (talker_unique_id < AVB_1722_1_MAX_TALKERS)
	{
		for (unsigned i=0; i<6; ++i)
			acmp_talker_streams[talker_unique_id].destination_mac[i] = macaddr[i];
	}
}

void avb_1722_1_talker_set_stream_id(unsigned talker_unique_id, unsigned streamId[2])
{
	if (talker_unique_id < AVB_1722_1_MAX_TALKERS)
	{
		acmp_talker_streams[talker_unique_id].stream_id.l = (unsigned long long)streamId[1] + (((unsigned long long)streamId[0]) << 32);
	}
}

// TODO: These functions are not currently invoked
avb_1722_1_acmp_status_t avb_1722_1_acmp_configure_source(unsigned talker_unique_id, unsigned int default_format)
{
	unsigned number_of_channels;
	unsigned sample_rate;
	unsigned state=0;
	int associated_clock=0;

	int channel_index = qlog2(default_format & 0xFFFF);
	int rate_index = qlog2((default_format&0xFC000000)>>26);
	if (channel_index==-1 || rate_index==-1) return ACMP_STATUS_TALKER_DEFAULT_FORMAT_INVALID;

	number_of_channels = avb_1722_1_acmp_default_format_channel_counts[channel_index];
	sample_rate = avb_1722_1_acmp_default_format_frequency[rate_index];

	if (talker_unique_id > AVB_NUM_LISTENER_UNITS) return ACMP_STATUS_TALKER_UNKNOWN_ID;

	if (number_of_channels > AVB_NUM_MEDIA_INPUTS)
	{
		return ACMP_STATUS_TALKER_DEFAULT_FORMAT_INVALID;
	}

	get_avb_source_sync(talker_unique_id, &associated_clock);

	get_avb_source_state(talker_unique_id, &state);
	if (state != AVB_SOURCE_STATE_DISABLED)
	{
		int current_number_of_channels=0;
		int current_sample_rate=0;
		get_device_media_clock_rate(associated_clock, &current_sample_rate);
		get_avb_source_channels(talker_unique_id, &current_number_of_channels);
		if (sample_rate != current_sample_rate) return ACMP_STATUS_DEFAULT_SET_DIFFERENT;
		if (number_of_channels != current_number_of_channels) return ACMP_STATUS_DEFAULT_SET_DIFFERENT;
	}
	else
	{
		int channel_map[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
		set_avb_source_channels(talker_unique_id, number_of_channels);
		set_avb_source_format(talker_unique_id, AVB_SOURCE_FORMAT_MBLA_24BIT, sample_rate);
		set_avb_source_map(talker_unique_id, channel_map, number_of_channels);
		set_device_media_clock_rate(associated_clock, sample_rate);
	}
	return ACMP_STATUS_SUCCESS;
}

avb_1722_1_acmp_status_t avb_1722_1_acmp_configure_sink(unsigned listener_unique_id, unsigned int default_format)
{
	unsigned number_of_channels;
	unsigned sample_rate;
	unsigned state=0;
	int associated_clock=0;

	int channel_index = qlog2(default_format & 0xFFFF);
	int rate_index = qlog2((default_format&0xFC000000)>>26);
	if (channel_index==-1 || rate_index==-1) return ACMP_STATUS_LISTENER_DEFAULT_FORMAT_INVALID;

	number_of_channels = avb_1722_1_acmp_default_format_channel_counts[channel_index];
	sample_rate = avb_1722_1_acmp_default_format_frequency[rate_index];

	if (listener_unique_id > AVB_NUM_LISTENER_UNITS) return ACMP_STATUS_LISTENER_UNKNOWN_ID;

	if (number_of_channels > AVB_NUM_MEDIA_OUTPUTS)
	{
		return ACMP_STATUS_LISTENER_DEFAULT_FORMAT_INVALID;
	}

	get_avb_sink_sync(listener_unique_id, &associated_clock);

	get_avb_source_state(listener_unique_id, &state);
	if (state != AVB_SINK_STATE_DISABLED)
	{
		int current_number_of_channels=0;
		int current_sample_rate=0;
		get_device_media_clock_rate(associated_clock, &current_sample_rate);
		get_avb_sink_channels(listener_unique_id, &current_number_of_channels);
		if (sample_rate != current_sample_rate) return ACMP_STATUS_DEFAULT_SET_DIFFERENT;
		if (number_of_channels != current_number_of_channels) return ACMP_STATUS_DEFAULT_SET_DIFFERENT;
	}
	else
	{
		int channel_map[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
		set_avb_sink_channels(listener_unique_id, number_of_channels);
		set_avb_sink_map(listener_unique_id, channel_map, number_of_channels);
		set_device_media_clock_rate(associated_clock, sample_rate);
	}
	return ACMP_STATUS_SUCCESS;
}

#undef CONTROLLER
#undef LISTENER

#undef TRUE
#undef FALSE
