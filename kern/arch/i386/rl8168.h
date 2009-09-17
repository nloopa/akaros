#ifndef ROS_INC_REALTEK_H
#define ROS_INC_REALTEK_H

#include <ros/common.h>
#include <trap.h>
#include <pmap.h>

#define rl8168_debug(...)  //cprintf(__VA_ARGS__)  
#define rl8168_interrupt_debug(...) //cprintf(__VA_ARGS__)  
#define rl8168_frame_debug(...)  //cprintf(__VA_ARGS__)  

// We need to provide some global interface for sending and receiving packets to a generic interface
//  for now, that is this set of Macros! They aren't in caps since the final inteface shoudn't be.
#define packet_wrap rl8168_packet_wrap
#define send_frame rl8168_send_frame

#define NE2K_IRQ_CPU		5

#define REALTEK_VENDOR_ID   0x10ec
#define REALTEK_DEV_ID      0x8168

// Realtek Offsets
#define RL_HWREV_REG		0x40
#define RL_MAC_OFFSET		0x00
#define RL_CTRL_REG         0x37
#define RL_IM_REG			0x3c
#define RL_IS_REG			0x3E
#define RL_EP_CTRL_REG		0x50
#define RL_RX_CFG_REG 		0x44
#define RL_TX_CFG_REG		0x40
#define RL_RX_MXPKT_REG     0xDA
#define RL_TX_MXPKT_REG     0xEC
#define RL_RX_DES_REG       0xE4
#define RL_TX_DES_REG       0x20
#define RL_TX_CTRL_REG		0x38	
#define RL_CP_CTRL_REG		0xE0		

// Realtek masks
#define RL_HWREV_MASK		0x7C800000
#define RL_CTRL_RXTX_MASK	0x0C
#define RL_CTRL_RESET_MASK  0x10

#define RL_EP_CTRL_UL_MASK	0xC0
#define RL_EP_CTRL_L_MASK	0x00
#define RL_TX_SEND_MASK		0x40
#define RL_CP_MAGIC_MASK	0x002B // Magic bits pulled from the BSD driver.
								   // Are listed as needed for TX/RX checksumming

// NOTE: THESE SHOULD BE BROKEN DOWN INTO A SERIES OF BITS TO REPERSENT THE VARIOUS OPTIONS
// AND THEN THE MASK SHOULD BE DEFINED TO BE AN OR OF THOSE BITS. THIS IS A QUICK HACK JOB.
// See interrupts below for how this should be done
#define RL_RX_CFG_MASK		0x0000E70F  // RXFTH: unlimited, MXDMA: unlimited, AAP: set (promisc. mode set)
#define RL_TX_CFG_MASK		0x3000700  // IFG: normal, MXDMA: unlimited.  crc appended

// Realtek interrupt bits
#define RL_INT_SERR			0x8000
#define RL_INT_TIMEOUT		0x4000
#define RL_INT_SWINT		0x0100
#define RL_INT_TDU			0x0080
#define RL_INT_FOVW			0x0040
#define RL_INT_LINKCHG		0x0020
#define RL_INT_RDU			0x0010
#define RL_INT_TERR			0x0008
#define RL_INT_TOK			0x0004
#define RL_INT_RERR			0x0002
#define RL_INT_ROK			0x0001

#define RL_INTERRUPT_MASK 	RL_INT_LINKCHG | RL_INT_TOK | RL_INT_ROK | RL_INT_SWINT
#define RL_INTRRUPT_CLEAR	0xFFFF

// Realtek descriptor command bits
#define DES_OWN_MASK		0x80000000
#define DES_EOR_MASK		0x40000000
#define DES_RX_SIZE_MASK	0x3FFF
#define DES_FS_MASK			0x20000000
#define DES_LS_MASK			0x10000000
#define DES_MAR_MASK		0x08000000
#define DES_PAM_MASK		0x04000000
#define DES_BAR_MASK		0x02000000

// TFor some reaosn the bits are in an undocumented position for our NIC
// They should be part of the command field, at the commented addrs below. instead
// they are part of the vlan field as stated below.
//#define DES_TX_IP_CHK_MASK  0x40000
//#define DES_TX_UDP_CHK_MASK 0x20000
//#define DES_TX_TCP_CHK_MASK 0x10000
#define DES_TX_IP_CHK_MASK  0x20000000
#define DES_TX_UDP_CHK_MASK 0x80000000
#define DES_TX_TCP_CHK_MASK 0x40000000

// Offset used for indexing IRQs
#define KERNEL_IRQ_OFFSET	32

// Basic packet sizing
// TODO handle jumbo packets
#define ETHERNET_ENCAP_SIZE	18
#define MTU					1500	
#define MAX_FRAME_SIZE		ETHERNET_ENCAP_SIZE + MTU	
	
// Realtek Descriptor Related Sizing
#define NUM_TX_DESCRIPTORS	1024
#define NUM_RX_DESCRIPTORS	1024

// !!!!!!!!! need to verify the 128byte nature of this field. Spec says it could be 32 for some chips.

#define RL_TX_MAX_BUFFER_SIZE  ROUNDUP(MAX_FRAME_SIZE, 128)
#define RL_RX_MAX_BUFFER_SIZE  ROUNDUP(MAX_FRAME_SIZE, 8)    // Might be able to be 4 less. Does it strip out the crc flag?

#define RL_TX_MAX_SIZE		RL_TX_MAX_BUFFER_SIZE / 128
#define RL_RX_MAX_SIZE		RL_RX_MAX_BUFFER_SIZE

#define RL_DES_ALIGN	256
#define RL_BUF_ALIGN	8

// ^----- Good line ------^

// v----- Evil line ------v
// Hacky stuff for syscalls go away.

struct ETH_Header
{
	char dest_mac[6];
	char source_mac[6];
	uint16_t eth_type;
};


struct IP_Header
{
	uint32_t ip_opts0;
	uint32_t ip_opts1;
	uint32_t ip_opts2;
	uint32_t source_ip;
	uint32_t dest_ip;
};

struct UDP_Header
{
	uint16_t source_port;
	uint16_t dest_port;
	uint16_t length;
	uint16_t checksum;
};	

#define MINIMUM_PACKET_SIZE 14 // kinda evil. probably evil.
#define MAX_PACKET_SIZE		MTU

#define PACKET_HEADER_SIZE  sizeof(struct rl8168_header) //IP UDP ETH
#define MAX_PACKET_DATA		MAX_FRAME_SIZE - PACKET_HEADER_SIZE
// This number needs verification! Also, this is a huge hack, as the driver shouldnt care about UDP/IP etc.

struct rl8168_header {
	struct ETH_Header eth_head;
	struct IP_Header ip_head;
	struct UDP_Header udp_head;
};

struct rl8168_packet {
	struct rl8168_header rl8168_head;
	char data[MTU-PACKET_HEADER_SIZE];
} __attribute__((packed));

char *CT(PACKET_HEADER_SIZE + len)
rl8168_packet_wrap(const char *CT(len) data, size_t len);


// ^----- Evil line ------^

// v----- Good line ------v


void rl8168_init(void);
void rl8168_reset(void);
void rl8168_interrupt_handler(trapframe_t *tf, void* data);
int rl8168_scan_pci(void);
void rl8168_read_mac(void);
void rl8168_setup_interrupts(void);
void rl8168_setup_descriptors(void);
void rl8168_configure(void);
void rl8168_handle_rx_packet(void);
void rl8168_set_rx_descriptor(uint32_t des_num, uint8_t reset_buffer);
void rl8168_set_tx_descriptor(uint32_t des_num);
void rl8168_process_frame(char *CT(frame_size) frame_buffer,
                          uint32_t frame_size, uint32_t command);
int rl8168_send_frame(const char *CT(len) data, size_t len);

#endif /* !ROS_INC_REALTEK_H */
