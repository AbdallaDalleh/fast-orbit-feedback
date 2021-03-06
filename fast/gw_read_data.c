#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <endian.h>
#include <time.h>
#include <signal.h>

#include <pevulib.h>

#define BPM_COUNT			32
#define NUMBER_OF_CHANNELS	12
#define BASE_CHANNEL_OFFSET 0x100
#define EVENT_ID            0x40
#define EVENT_ID_BITS		0xf0
#define EVENT_CHANNEL_BITS	0x0f
#define REGISTER_OPERATION_WRITE	0x0080000000000000ULL	// b56 == "w/!r"
#define REGISTER_OPERATION_READ		0x0000000000000000ULL	// b56 == "w/!r"
#define REGISTER_LINK_PS_SHIFT		46
#define TSR_ERROR					0x8000
#define ADDRESS_SET_REF		175
#define ADDRESS_ILOAD		153

#define PEV_OK				0
#define PEV_ERR_CHANNEL 	1
#define PEV_ERR_REGISTER	2

// Registers IDs in the memory map from PEV.
// These registeres are transferred in the memory map.
enum
{
	REGISTER_PRIORITY_WRITE	= 0,
	REGISTER_NORMAL_WRITE,
	REGISTER_NORMAL_READ,
	REGISTER_WAVEFORM_WRITE,
	REGISTER_WAVEFORM_READ,
	NUMBER_OF_IO_REGISTERS,	/*Reserved*/
	REGISTER_MSR,
	REGISTER_TSR,
	NUMBER_OF_REGISTERS
};

// A struct to define the PEV memory map transferred to the PSC.
// Each channel contains 8 unsigned 64-bit registeres.
typedef struct
{
	uint64_t registers[NUMBER_OF_REGISTERS];
} channel_t;

// This struct defines the mapping of the correctors to the PSC controllers.
// Example: Corrector #4 is on channel 1 PS 1
struct psc_map_t
{
	uint32_t channel;
	uint32_t ps;
} psc_map[BPM_COUNT] = {
	{0, 1}, {0, 2}, {0, 3}, {1, 1},  {1, 2},  {1, 3},  {2, 1},  {2, 2},	
	{3, 1}, {3, 2}, {3, 3},	{4, 1},  {4, 2},  {4, 3},  {5, 1},  {5, 2},	
	{6, 1}, {6, 2}, {6, 3},	{7, 1},  {7, 2},  {7, 3},  {8, 1},  {8, 2},
	{9, 1}, {9, 2}, {9, 3},	{10, 1}, {10, 2}, {10, 3}, {11, 1}, {11, 2}
};

// const uint64_t iload_address = (uint64_t)(157);
float psc_iloads[BPM_COUNT];

// Declarations for PEV initialization.
static channel_t* channels;
static struct pev_ioctl_map_pg map;
static struct pev_node *node;
static struct pev_ioctl_evt *event;
static void	*base;

static void initialize_pev();
static void cleanup_pev(int code);
static void initialize_values();
static int  pev_read(u32 channel, u64 address, u64 ps, float* value);
static int  pev_write(u32 channel, u64 address, u64 ps, float value);

int main(int argc, char** argv)
{
	int status;
	int socket_fd;
	struct sockaddr_in sa;
	float buffer[BPM_COUNT];
	ssize_t bytes;
	uint32_t channel;
	uint32_t raw_value;
	uint64_t address;
	uint64_t ps;
	float current_load;

	initialize_pev();
	initialize_values();

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(55555);

	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(bind(socket_fd, (struct sockaddr*) &sa, sizeof(sa)) == -1)
	{
		perror("bind");
		close(socket_fd);
		return 1;
	}

	signal(SIGINT, cleanup_pev);

	int l = 0;
	while(l++ < 10)
	{
		printf("Waiting for buffer ...\n");
		bytes = read(socket_fd, &buffer, sizeof(buffer));
		if(bytes < 0)
		{
			perror("recv error");
			exit(99);
		}

		int i = 0;
		for(i = 0; i < BPM_COUNT; i++)
		{
			// Test the received data.
			printf("%.3f\n", buffer[i]);

			// Perform correction on the current values we stored initially.
			psc_iloads[i] += buffer[i];

			// Write these values to the memory map.
			ps = (uint64_t)psc_map[i].ps << REGISTER_LINK_PS_SHIFT;
			address = (uint64_t) ADDRESS_SET_REF;
			channel = psc_map[i].channel;
			raw_value = *(uint32_t*)(&psc_iloads[i]);

			pev_write(channel, address, ps, current_load);
		}
	}

	return 0;
}

void initialize_pev()
{
	int status;

	node = pev_init(0);
	if(!node) 
	{
		printf("[psc][pev] Initialization failed");
		return;
	}
	if(node->fd < 0) 
	{
		printf("[psc][pev] Can't find PEV1100 interface");
		return;
	}

	/*
	 * Map PSC registers
	 */
	map.rem_addr = 0;
	map.mode  = MAP_ENABLE | MAP_ENABLE_WR | MAP_SPACE_USR1;
	map.flag  = 0;
	map.sg_id = MAP_MASTER_32;
	map.size  = 0x400000;
	status    = pev_map_alloc(&map);

	base = pev_mmap(&map);
	channels = (channel_t*)(base + BASE_CHANNEL_OFFSET);

	/*
	 * Allocate event queue, register channel event, and enable it
	 */
	event = pev_evt_queue_alloc(0);
	if(!event)
	{ 
		printf("[psc][pev] Can't allocate event queue");
		return;
	}
	event->wait = -1;

	int c = 0;
	for(c = 0; c < NUMBER_OF_CHANNELS; c++)
	{
		status = pev_evt_register (event, EVENT_ID + c);
		if(status)
		{
			printf("[psc][pev] Event queue register failed");
			return;
		}	
		status = pev_evt_queue_enable(event);
		if(status)
		{
			printf("[psc][pev] Event queue enable failed");
			return;
		}
	}
}

void cleanup_pev(int code)
{
	uint32_t channel;

	printf("\n\rSIGINT captured, cleanup in progress ... \n");
	
	pev_evt_read(event, 1);
	pev_evt_unmask(event, event->src_id);

	if(base)
	{
		pev_map_free(&map);
	}

	if(event)
	{
		pev_evt_queue_disable(event);
		for (channel = 0; channel < NUMBER_OF_CHANNELS; channel++)
			pev_evt_unregister (event, EVENT_ID+channel);

		pev_evt_queue_free(event);
	}

	int status = pev_exit(node);
	printf("Status: %d\n", status);
	printf("Done.\n");
	exit(0);
}

void initialize_values()
{
	float value;
	int c = 0;
	int status;
	uint32_t channel;
	uint32_t raw;
	uint64_t ps;
	uint64_t address;
	for(c = 0; c < BPM_COUNT; c++)
	{
		address = (uint64_t) ADDRESS_ILOAD;
		ps = ((uint64_t)psc_map[c].ps) << REGISTER_LINK_PS_SHIFT;
		channel = psc_map[c].channel;

		pev_read(channel, address, ps, &value);
		psc_iloads[c] = value;
	}
}

int pev_read(u32 channel, u64 address, u64 ps, float* value)
{
	uint32_t raw, status;
	float _value;

	channels[channel].registers[REGISTER_NORMAL_READ]  = REGISTER_OPERATION_READ  | ps | address << 32 | address;
	pev_evt_read(event, -1);
	pev_evt_unmask(event, event->src_id);
	if ((event->src_id & EVENT_ID_BITS) != EVENT_ID)
		return PEV_ERR_CHANNEL;

	channel = event->src_id & EVENT_CHANNEL_BITS;
	status = (uint32_t)channels[channel].registers[REGISTER_TSR];
	if (status & TSR_ERROR)
		return PEV_ERR_REGISTER;

	raw = (uint32_t)(channels[channel].registers[REGISTER_NORMAL_READ] & 0x00000000ffffffffULL);
	_value = *(float*)&raw;
	*value = _value;
	return PEV_OK;
}

int pev_write(u32 channel, u64 address, u64 ps, float value)
{
	u32 raw = *(u32*) &value;
	u32 status;

	channels[channel].registers[REGISTER_NORMAL_WRITE] = REGISTER_OPERATION_WRITE | ps | address << 32 | (uint64_t) raw;
	pev_evt_read(event, -1);
	pev_evt_unmask(event, event->src_id);
	if ((event->src_id & EVENT_ID_BITS) != EVENT_ID)
		return PEV_ERR_CHANNEL;

	channel = event->src_id & EVENT_CHANNEL_BITS;
	status = (uint32_t)channels[channel].registers[REGISTER_TSR];
	if (status & TSR_ERROR)
		return PEV_ERR_REGISTER;

	return PEV_OK;
}
