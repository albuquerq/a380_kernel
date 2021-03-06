#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/jzsoc.h>
#include <linux/kthread.h>




#define SPI_CS  (32 * 4 + 18) 
#define SPI_CLK (32 * 5 + 10)
#define SPI_DO  (32 * 5 + 11)
#define SPI_DI  (32 * 5 + 12)

#define GPIO_PKT_INT  (32 * 3 + 26)
#define GPIO_FIFO_INT (32 * 3 + 27)
#define GPIO_RESET (32 * 4 + 19)

#define PIN_POWER_ELAN (32*3 + 13)
#define ELAN_PKT_IRQ  (IRQ_GPIO_0 + GPIO_PKT_INT)

#define ELAN_NAME "elan_2d4g"
#define ELAN_MINOR 32   /* MAJOR: 10, MINOR: 32 */

//ioctls
#define ELAN_SET_MODE_HOST 0x01
#define ELAN_SET_MODE_CLIENT 0x02
#define ELAN_RECV_SEND   0x03
#define ELAN_SEND_RECV   0x04 
#define ELAN_SEND_VAL    0x05
#define ELAN_SEND_VAL_BLK    0x051
#define ELAN_GET_VAL     0x06
#define ELAN_CHECK_SYNC  0x07
#define ELAN_HW_ON		 0x08
#define ELAN_HW_OFF		 0x09
#define ELAN_SET_MAX_CLIENTS  0x10
#define ELAN_GET_HOST_STATE     0x11
#define ELAN_GET_CLIENT_STATE   0x12

#define ELAN_ENTER_TEST_RECV   0x13
#define ELAN_ENTER_TEST_SEND   0x14

#define ELAN_HW_RESET  0x15

//cmds
#define CMD_DATA         0xff
#define CMD_CONNECT_REQ  0x01
#define CMD_CONNECT_ACK  0x02
#define CMD_START_GAME   0x03
#define CMD_LEAVE_GAME   0x04   //client exit
#define CMD_EXIT_GAME    0x05   //all exit
#define CMD_TEST_DATA    0x06


#define __HOST__    1
//#define __CLIENT__  1

#define CLK_HZ 20
#define BASE_ADDR 20

static int sync_frame=0xffffffff;
static struct task_struct *elan_rx_task = NULL;
static spinlock_t elan_lock;

//===============   ARGS NEED TO BE INIT  ====================
static int machine_mode; //1:host  0:client
static unsigned char g_recv_data[14] = {0xff};
static unsigned int max_clients;
static int state = 0;
static int exit_game_flag = 0;
static int task_run = 0;
static int error_flag = 0;

typedef struct
{
	unsigned char len_send;
	unsigned char data_send[16];
	unsigned char len_recv;
	unsigned char data_recv[16];
}elan_packet_t;

//TXADR = 0x1100
//RXADR0 = 0x1100
//RXADR1 = 0x3322,  RXADR2 = 0x4422
static unsigned char frame_table[]={0X4E,0X02,0X4D,0X01,0X42,0X98,0X43,0XC4,0X44,0X06,
						 0X45,0X10,0X46,0X09,0X47,0X31,0X48,0X01,0X49,0X8A,
						 0X4A,0X27,0X4B,0X00,0X4C,0X06,0X50,0X00,0X51,0X11,
						 0X52,0X22,0X53,0X33,0X54,0X44,0X55,0X55,0X56,0X66,
						 0X57,0X77,0X58,0X08,0X00,0XE5,0X01,0X84,0X02,0X00,
						 0X03,0XC6,0X04,0X00,0X05,0X40,0X06,0X5D,0X07,0X18,
						 0X08,0X40,0X09,0X18,0X0A,0X47,0X0B,0X0B,0X0C,0XE0,
						 0X0D,0X4F,0X0E,0X11,0X0F,0X1C,0X20,0XAD,0X21,0X64,
						 0X22,0X00,0X23,0XC3,0X24,0XBD,0X25,0XA2,0X26,0X1A,
						 0X27,0X09,0X28,0X00,0X29,0XB8,0X2A,0X71,0X2B,0X06,
						 0X2C,0X80,0X2D,0X1A,0X2E,0X03,0X2F,0X64,0X30,0XC0,
						 0X31,0X00,0X32,0X40,0X33,0X3B,0X00,0XA7,0X32,0X4A,
						 0X00,0XE5,0X0E,0X91,0X40,0X51,0X41,0X81,0X0C,0XC0,
						 0X02,0X80,0X04,0X4A,0X05,0XDA,0X05,0XFA,0XFF};

static unsigned char frame_table2[]={0X05,0X40,0X02,0X00,0X0C,0XE0,0XFF};
//static unsigned char test_table[]={0X07,0X58,0X0E,0X11,0X2E,0X23,0X0E,0X91,0XFF};
//static unsigned char resume_table[]={0x07,0x18,0x0E,0X11,0X2E,0X03,0X0E,0X91,0XFF};
static unsigned char check_frametable[]={0X4E,0X02,0X43,0XC4,0X44,0X06,0X45,0X10,
							 0X48,0X01,0X4C,0X06,0X50,0X00,0X51,0X11,
							 0X52,0X22,0X53,0X33,0X54,0X44,0X55,0X55,
							 0X56,0X66,0X57,0X77,0X58,0X08,0XFF};


static volatile int delay_flag;
static void xdelay(int count)
{
	count = count * 6;
#if 1
	for(delay_flag=0; delay_flag<count; delay_flag++)
			;
#endif
	//udelay(1);
}



//============================================================================
static void jz_spi_init()
{
#if 0
	__gpio_as_ssi();

	//config spi
	//__cpm_ssiclk_select_pllout(); // pllout: 360 MHz 
	//__cpm_ssiclk_select_exclk(); // pllout: 360 MHz 
	__cpm_set_ssidiv(1);	// ssi source clk = 360 /(n+1) = 180 MHz 
	REG_SSI_GR = 11;	// clock = (180)/(2*(n+1)) MHz   // 7.5  MHz
	__cpm_start_ssi();

	__ssi_disable();
	__ssi_disable_tx_intr();
	__ssi_disable_rx_intr();
	__ssi_enable_receive();
	__ssi_flush_fifo();
	__ssi_clear_errors();

	__ssi_set_spi_clock_phase(0);     //PHA = 0
	__ssi_set_spi_clock_polarity(0);  //POL = 0
	__ssi_set_msb();
	__ssi_set_frame_length(8);
	__ssi_disable_loopback(); // just for test 
	//__ssi_enable_loopback(); // just for test 
	__ssi_select_ce();

	__ssi_set_tx_trigger(112); //n(128 - DMA_BURST_SIZE), total 128 entries 
	__ssi_set_rx_trigger(16);  //n(DMA_BURST_SIZE), total 128 entries 
	__ssi_set_msb();
	__ssi_spi_format();

	__ssi_enable();
#endif
	
//simulate SPI
	__gpio_as_output(SPI_CS);
	__gpio_set_pin(SPI_CS);

	__gpio_as_output(SPI_CLK);
	__gpio_clear_pin(SPI_CLK);

	__gpio_as_output(SPI_DO);
	__gpio_clear_pin(SPI_DO);		
	
	__gpio_as_output(SPI_DI);
	__gpio_clear_pin(SPI_DI);		
	
	__gpio_as_input(SPI_DI);
	__gpio_enable_pull(SPI_DI);

	__gpio_as_input(GPIO_PKT_INT);
}

static void jz_spi_deinit()
{
#if 0
	__ssi_disable();
	__cpm_stop_ssi();
#endif
}

#if 0
static unsigned char spi_rw_char(unsigned char wr_data)
{	
	unsigned char rd_data;

	__ssi_flush_rxfifo();
	__ssi_flush_txfifo();

	while(__ssi_txfifo_full()) ;

	__ssi_transmit_data(wr_data);
	
	while(__ssi_rxfifo_empty());
	
	rd_data = (unsigned char)(__ssi_receive_data());

	return rd_data;
}

static  void spi_write_char(unsigned char wr_data)
{	
	int i;	

	__gpio_clear_pin(SPI_CS);
	__gpio_set_pin(SPI_CLK);
	__gpio_clear_pin(SPI_DO);	

	for(i=0; i<8; i++)
	{
			if(wr_data & 0x80)	
					__gpio_set_pin(SPI_DO);	
			else
					__gpio_clear_pin(SPI_DO);
		
			__gpio_clear_pin(SPI_CLK);
			wr_data = (wr_data << 1);
			udelay(5);
			__gpio_set_pin(SPI_CLK);
			udelay(5);
	}

	__gpio_clear_pin(SPI_CLK);
	__gpio_set_pin(SPI_CS);
}

static unsigned char spi_read_char()
{
	int i;
	unsigned char val = 0;

	__gpio_set_pin(SPI_CLK);
	__gpio_clear_pin(SPI_DO);	

	__gpio_clear_pin(SPI_CS);
	__gpio_set_pin(SPI_CLK);
	for(i=0; i<8; i++)
	{
		udelay(5);
		__gpio_clear_pin(SPI_CLK);
		udelay(5);
		val |= __gpio_get_pin(SPI_DI);
		val = (val<<1);
		__gpio_set_pin(SPI_CLK);
	}

	__gpio_set_pin(SPI_CS);
	__gpio_set_pin(SPI_CLK);
	
	return val;
}


#endif


//============================================================================
#if 0
static void em198850_reg_write(unsigned char addr, unsigned char data)
{
 	spi_write_char(addr);
	udelay(5);
	spi_write_char(data);
}

static unsigned char em198850_reg_read(unsigned char addr)
{
	unsigned char data;
 	spi_write_char(addr | 0x80);
	udelay(5);
	data = spi_read_char();
	return data;
}
#endif

static void em198850_reg_write(unsigned char addr, unsigned char data)
{
	int i;	

	__gpio_clear_pin(SPI_CS);
	__asm__(
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t");

	for(i=0; i<8; i++)
	{
			if(addr & 0x80)	
					__gpio_set_pin(SPI_DO);	
			else
					__gpio_clear_pin(SPI_DO);
		
			xdelay(CLK_HZ);
			__gpio_set_pin(SPI_CLK);
			addr = (addr << 1);
			xdelay(CLK_HZ);
			__gpio_clear_pin(SPI_CLK);
	}


	for(i=0; i<8; i++)
	{
			if(data & 0x80)	
					__gpio_set_pin(SPI_DO);	
			else
					__gpio_clear_pin(SPI_DO);
		
			xdelay(CLK_HZ);
			__gpio_set_pin(SPI_CLK);
			data = (data << 1);
			xdelay(CLK_HZ);
			__gpio_clear_pin(SPI_CLK);
	}

	__gpio_clear_pin(SPI_CLK);
	__gpio_set_pin(SPI_CS);
}

static unsigned char em198850_reg_read(unsigned char addr)
{
	int i;
	unsigned char val = 0;

	addr |= 0x80;
	__gpio_clear_pin(SPI_CS);
	__asm__(
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t");

	for(i=0; i<8; i++)
	{
			if(addr & 0x80)	
					__gpio_set_pin(SPI_DO);	
			else
					__gpio_clear_pin(SPI_DO);
		
			xdelay(CLK_HZ);
			__gpio_set_pin(SPI_CLK);
			addr = (addr << 1);
			xdelay(CLK_HZ);
			__gpio_clear_pin(SPI_CLK);
	}

	for(i=0; i<8; i++)
	{
		xdelay(CLK_HZ);
		__gpio_set_pin(SPI_CLK);
		xdelay(CLK_HZ);
		val = (val<<1);
		val |= __gpio_get_pin(SPI_DI);
		__gpio_clear_pin(SPI_CLK);
	}

	__gpio_clear_pin(SPI_CLK);
	__gpio_set_pin(SPI_CS);
	
	return val;
}


static void em198850_reg_init1()
{
	unsigned char idx,addr,data;
	idx = 0;

	while(frame_table[idx] != 0xFF)
	{
		addr = frame_table[idx++];
		data = frame_table[idx++];
		em198850_reg_write(addr, data);
	}
}

static void em198850_reg_init2()
{
	unsigned char idx,addr,data;
	idx = 0;

	while(frame_table2[idx] != 0xFF)
	{
		addr = frame_table[idx++];
		data = frame_table[idx++];
		em198850_reg_write(addr, data);
	}
}

static void rssi_init()
{
	unsigned char r_big,idx,data;
	r_big = 0;
	idx = 0;
	while(idx != 5)
	{
		idx++;
		data = em198850_reg_read(0x4B);
		if(r_big < data)	
		{
			r_big = data;
		}
	}
	//printk("rssi_init, r_big = %x\n",r_big);
	r_big = r_big - 4;
	em198850_reg_write(0x4A, r_big);
}

static unsigned char em198850_reg_test()
{
	unsigned char idx,addr,data,ret;
	idx = 0;
	ret = 1;

	while( check_frametable[idx] != 0xFF )
	{
		addr  = check_frametable[idx++];
		data = em198850_reg_read(addr);
		//printk("addr = 0x%x, data = 0x%x\n",addr,data);
#if 1
		if(data != check_frametable[idx++])	
		{
			ret = 0;
			break;
		}
#endif
	}	
	return ret;
}

//===========================================================================
static void rf_enter_tx_noack_nocrc_payload5()
{
	em198850_reg_write(0x40, 0x52);  //TX no ack
	em198850_reg_write(0x41, 0x80);
	em198850_reg_write(0x43, 0x84);
	em198850_reg_write(0x44, 0x05);
	em198850_reg_write(0x45, 0x10);
	em198850_reg_write(0x47, 0x01);
	//udelay(100);
}

#if 0
//0X47,0X31,0X48,0X01,0X49,0X8A,
static void rf_enter_tx_ack_crc_payload5()
{
	em198850_reg_write(0x40, 0x56);  //TX Need ACK
	em198850_reg_write(0x41, 0x80);	 
	em198850_reg_write(0x43, 0xA4);  // 1 BYTE CRC
	em198850_reg_write(0x44, 0x05);
	em198850_reg_write(0x45, 0x10);
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
	//udelay(100);
}
#endif
static void rf_enter_tx_ack_crc_payload4()
{
	em198850_reg_write(0x40, 0x56);  //TX Need ACK
	em198850_reg_write(0x41, 0x80);	 
	em198850_reg_write(0x43, 0xA4);  // 1 BYTE CRC
	em198850_reg_write(0x44, 0x04);
	em198850_reg_write(0x45, 0x10);
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
	//udelay(100);
}


static void rf_enter_tx_ack_crc_payload5()
{
	em198850_reg_write(0x40, 0x56);  //TX Need ACK
	em198850_reg_write(0x41, 0x80);	 
	em198850_reg_write(0x43, 0xA4);  // 1 BYTE CRC
	em198850_reg_write(0x44, 0x05);
	em198850_reg_write(0x45, 0x10);
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
	//udelay(100);
}

static void rf_enter_tx_ack_crc_payload9()
{
	em198850_reg_write(0x40, 0x56);  //TX Need ACK
	em198850_reg_write(0x41, 0x80);	 
	em198850_reg_write(0x43, 0xA4);  // 1 BYTE CRC
	em198850_reg_write(0x44, 0x09);
	em198850_reg_write(0x45, 0x10);
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
	//udelay(100);
}

static void rf_enter_tx_ack_crc_payload14()
{
	em198850_reg_write(0x40, 0x5E);  //TX Need ACK
	em198850_reg_write(0x41, 0x80);	 
	em198850_reg_write(0x43, 0xC4);  // 2 BYTE CRC
	em198850_reg_write(0x44, 0x0E);
	em198850_reg_write(0x45, 0x10);
	em198850_reg_write(0x47, 0x31);  //ReTransmit 4 times 
	//em198850_reg_write(0x49, 0xFF);  
	//udelay(100);
}


static void rf_enter_rx_noack_nocrc_payload5()
{
	em198850_reg_write(0x40, 0x51);  //RX no ack
	em198850_reg_write(0x41, 0x81);
	em198850_reg_write(0x43, 0x84);  //crc = 0
	em198850_reg_write(0x44, 0x05);  //payload = 5
	em198850_reg_write(0x45, 0x10);	 //packet cnt = 1
	em198850_reg_write(0x47, 0x01);  //rtx cnt = 0
}

#if 0
static void rf_enter_rx_ack_crc_payload5()
{
	em198850_reg_write(0x40, 0x59);  //RX Auto ack
	em198850_reg_write(0x41, 0x81);
	em198850_reg_write(0x43, 0xA4);  //crc = 1 byte
	em198850_reg_write(0x44, 0x05);  //payload = 5
	em198850_reg_write(0x45, 0x10);	 //packet cnt = 1
	//em198850_reg_write(0x47, 0x01);  //rtx cnt = 0
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
}
#endif
static void rf_enter_rx_ack_crc_payload4()
{
	em198850_reg_write(0x40, 0x59);  //RX Auto ack
	em198850_reg_write(0x41, 0x81);
	em198850_reg_write(0x43, 0xA4);  //crc = 1 byte
	em198850_reg_write(0x44, 0x04);  //payload = 5
	em198850_reg_write(0x45, 0x10);	 //packet cnt = 1
	//em198850_reg_write(0x47, 0x01);  //rtx cnt = 0
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
}

static void rf_enter_rx_ack_crc_payload9()
{
	em198850_reg_write(0x40, 0x59);  //RX Auto ack
	em198850_reg_write(0x41, 0x81);
	em198850_reg_write(0x43, 0xA4);  //crc = 1 byte
	em198850_reg_write(0x44, 0x09);  //payload = 5
	em198850_reg_write(0x45, 0x10);	 //packet cnt = 1
	//em198850_reg_write(0x47, 0x01);  //rtx cnt = 0
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
}

static void rf_enter_rx_ack_crc_payload14()
{
	em198850_reg_write(0x40, 0x5D);  //RX Auto ack
	em198850_reg_write(0x41, 0x81);
	em198850_reg_write(0x43, 0xC4);  //crc = 2 byte
	em198850_reg_write(0x44, 0x0E);  //payload = 13
	em198850_reg_write(0x45, 0x10);	 //packet cnt = 1
	//em198850_reg_write(0x47, 0x01);  //rtx cnt = 0
	em198850_reg_write(0x47, 0x31);  //ReTransmit 4 times 
	//em198850_reg_write(0x49, 0xFF);  
}


static void rf_enter_rx_ack_crc_payload5()
{
	em198850_reg_write(0x40, 0x59);  //RX Auto ack
	em198850_reg_write(0x41, 0x81);
	em198850_reg_write(0x43, 0xA4);  //crc = 1 byte
	em198850_reg_write(0x44, 0x05);  //payload = 5
	em198850_reg_write(0x45, 0x10);	 //packet cnt = 1
	//em198850_reg_write(0x47, 0x01);  //rtx cnt = 0
	em198850_reg_write(0x47, 0xF1);  //ReTransmit 4 times 
}

static void rf_channel_set(unsigned char channel)
{
	em198850_reg_write(0x02, channel);
}

#define SOF 0x7F
static void write_fifo(unsigned char *data, unsigned char cnt)
{
	int i=0,j=0;
	unsigned char sof = SOF;	
	unsigned char tmp = 0;

	//printk("T: %x %x %x %x\n",data[0],data[1],data[2],data[3]);
	//printk("write fifo 0x%x 0x%x 0x%x 0x%x\n",data[0],data[1],data[2],data[3]);
	if(data == NULL) return; 	

#if 0
	spi_write_char(SOF);
	
	while(cnt--)
	{
		spi_write_char(data[i++]);	
	}
#endif

	__gpio_clear_pin(SPI_CS);
	__asm__(
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t");

	for(i=0; i<8; i++)
	{
			if(sof & 0x80)	
					__gpio_set_pin(SPI_DO);	
			else
					__gpio_clear_pin(SPI_DO);
		
			xdelay(CLK_HZ);
			__gpio_set_pin(SPI_CLK);
			sof = (sof << 1);
			xdelay(CLK_HZ);
			__gpio_clear_pin(SPI_CLK);
	}

	for(j=0; j<cnt; j++)
	{
			tmp = data[j];
			for(i=0; i<8; i++)
			{
					if(tmp & 0x80)	
							__gpio_set_pin(SPI_DO);	
					else
							__gpio_clear_pin(SPI_DO);

					xdelay(CLK_HZ);
					__gpio_set_pin(SPI_CLK);
					tmp = (tmp << 1);
					xdelay(CLK_HZ);
					__gpio_clear_pin(SPI_CLK);
			}
	}
	__gpio_clear_pin(SPI_CLK);
	__gpio_set_pin(SPI_CS);
}

static void read_fifo(unsigned char *data, unsigned char cnt)
{
	int i=0,j=0;
	unsigned char sof = SOF | 0x80;	
	unsigned char tmp = 0;
	if(data == NULL)	return; 	

	__gpio_clear_pin(SPI_CS);
	__asm__(
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t"
	"nop\n\t");

	for(i=0; i<8; i++)
	{
			if(sof & 0x80)	
					__gpio_set_pin(SPI_DO);	
			else
					__gpio_clear_pin(SPI_DO);
		
			xdelay(CLK_HZ);
			__gpio_set_pin(SPI_CLK);
			sof = (sof << 1);
			xdelay(CLK_HZ);
			__gpio_clear_pin(SPI_CLK);
	}

	for(j=0; j<cnt; j++)
	{
			tmp = 0;
			for(i=0; i<8; i++)
			{
					xdelay(CLK_HZ);
					__gpio_set_pin(SPI_CLK);
					xdelay(CLK_HZ);
					tmp = (tmp<<1);
					tmp |= __gpio_get_pin(SPI_DI);
					__gpio_clear_pin(SPI_CLK);
			}
			data[j] = tmp;
	}
	__gpio_clear_pin(SPI_CLK);
	__gpio_set_pin(SPI_CS);
	//printk("R: %x %x %x %x\n",data[0],data[1],data[2],data[3]);
	//printk("%d--r: 0x%x,0x%x,0x%x,0x%x\n",cnt, data[0],data[1],data[2],data[3]);
}

static void wait_pkg_high()
{
	while(__gpio_get_pin(GPIO_PKT_INT) == 0);
}

static int wait_pkg_high_timeout(int usec)
{
	while((usec--)>0)
	{
		if(__gpio_get_pin(GPIO_PKT_INT))
			return 0;
		xdelay(200);
	}
	return 1;
}

static int lost_packet()
{
	static unsigned char old_data=0;
    unsigned char data;
	data = em198850_reg_read(0x4F);
	data = (data >> 3);
	if((data - old_data) == 1)
	{
			old_data = data;
			//printk("lost,data=%d\n",data);
			if(data == 31)	
			{
				em198850_reg_write(0x4F, 0);
				old_data = 0;		
			}
			return 1;
	}	
	return 0;
}

#if 0
static void client_test()
{
	unsigned char send_data[5] = {0, 0, 0, 0, 0};
	unsigned char recv_data[5] = {0, 0, 0, 0, 0};
	unsigned int tt = 0;	
	int i=0;

	rf_channel_set(35);
	//rf_enter_tx_noack_nocrc_payload5();
	
	//Make Sure the HOST is RX Now!!
	rf_enter_tx_ack_crc_payload5();

	while(1)
	{
		recv_data[0] = 0;
		recv_data[1] = 0;
		recv_data[2] = 0;
		recv_data[3] = 0;
		recv_data[4] = 0;

		send_data[0] = (tt++) & 0xFF;
		send_data[1] = (tt++) & 0xFF;
		send_data[2] = (tt++) & 0xFF;
		send_data[3] = (tt++) & 0xFF;
		send_data[4] = (tt++) & 0xFF;
	
		mdelay(40);
		
		do
		{
				write_fifo(send_data, 5);
				wait_pkg_high();
		}while(lost_packet());
		
		//rf_enter_rx_noack_nocrc_payload5();
		rf_enter_rx_ack_crc_payload5();
		
		wait_pkg_high();
		read_fifo(recv_data, 5);
		
		if( (recv_data[0] == send_data[0])  &&
			(recv_data[1] == send_data[1])  &&
			(recv_data[2] == send_data[2])  &&
			(recv_data[3] == send_data[3])  &&
			(recv_data[4] == send_data[4])  
			)
		{
				//printk("ok,%d\n",i++);
				//rf_enter_tx_noack_nocrc_payload5();
				rf_enter_tx_ack_crc_payload5();
				udelay(50);  //Wait Host to RX mode
				send_data[0] = 0xFF;
				send_data[1] = 0xFF;
				send_data[2] = 0xFF;
				send_data[3] = 0xFF;
				send_data[4] = 0xFF;
		
				do{
				write_fifo(send_data, 5);
				wait_pkg_high();
				}while(lost_packet());
		}
		else
		{
				printk("error\n");
				//rf_enter_tx_noack_nocrc_payload5();
				rf_enter_tx_ack_crc_payload5();
				udelay(50);  //Wait Host to RX mode
				send_data[0] = 0xAA;
				send_data[0] = 0xAA;
				send_data[1] = 0xAA;
				send_data[2] = 0xAA;
				send_data[3] = 0xAA;
				send_data[4] = 0xAA;
			
				do{
				write_fifo(send_data, 5);
				wait_pkg_high();}while(lost_packet());
		}
	}

}

static void host_test()
{
	unsigned char send_data[5] = {0, 0, 0, 0, 0};
	unsigned char recv_data[5] = {0, 0, 0, 0, 0};

	rf_channel_set(35);
	//rf_enter_rx_noack_nocrc_payload5();
	rf_enter_rx_ack_crc_payload5();

	while(1)
	{
		//mdelay(20);
	
		wait_pkg_high();
		read_fifo(recv_data, 5);
		
		send_data[0] = recv_data[0];
		send_data[1] = recv_data[1];
		send_data[2] = recv_data[2];
		send_data[3] = recv_data[3];
		send_data[4] = recv_data[4];
		
		//mdelay(1);

		//rf_enter_tx_noack_nocrc_payload5();
		rf_enter_tx_ack_crc_payload5();
		printk("x\n");
		//udelay(50); //wait client change to RX mode
		do{
		write_fifo(send_data, 5);
		wait_pkg_high();}while(lost_packet());
				
		//mdelay(1);
		
		//rf_enter_rx_noack_nocrc_payload5();
		rf_enter_rx_ack_crc_payload5();
		wait_pkg_high();
		read_fifo(recv_data, 5);

		if(recv_data[0] == 0xFF)	
		{
		//printk("ok,0x%x\n",recv_data[0]);
		}
		else
		printk("error,0x%x,0x%x,0x%x,0x%x,0x%x\n",recv_data[0],recv_data[1],recv_data[2], recv_data[3],recv_data[4]);
	}
}
#endif

static void process_cmd()
{	
	printk("** 0x%x 0x%x 0x%x 0x%x **\n",g_recv_data[0], g_recv_data[1], g_recv_data[2], g_recv_data[3]);
	switch(g_recv_data[0])
	{
		//host recv
		case CMD_CONNECT_REQ:
		{
			//maybe
			if(machine_mode == 1) 
			{
					if(state<max_clients)
					{
							unsigned char send_data[14];
							state++;
							send_data[0] = CMD_CONNECT_ACK;
							send_data[1] = state+1;
							send_data[2] = BASE_ADDR + state;
							__gpio_as_input(GPIO_PKT_INT);
							rf_enter_tx_ack_crc_payload14();
							do{
									write_fifo(send_data, 14);
									wait_pkg_high();}
							while(lost_packet());

							rf_enter_rx_ack_crc_payload14();
							__gpio_as_irq_rise_edge(GPIO_PKT_INT);
					}
			}
		
			memset(g_recv_data, 0xff, 14);
			break;
		}	
	
		/*
		*  client recv
		*  data[0] = CMD_CONNECT_ACK;
		*  data[1] = xP;
		*  data[2] = Addr;
		*/
		case CMD_CONNECT_ACK:
		{
			if(machine_mode == 0) 
			{
				state = g_recv_data[1];
			}
			memset(g_recv_data, 0xff, 14);
			break;
		}
	
		/*
		*  client recv
		*  data[0] = CMD_CONNECT_ACK;
		*  data[1] = xP;
		*  data[2] = Addr;
		*/
		case CMD_START_GAME:
		{
			if(machine_mode == 0)
			{
				if(state>0) state |= 0x80;
			}
			memset(g_recv_data, 0xff, 14);
			break;
		}


		/*
		*  host recv
		*  data[0] = CMD_LEAVE_GAME;
		*  data[1] = xP;
		*  data[2] = Addr;
		*/
		case CMD_LEAVE_GAME:
		{
			if(machine_mode == 1)
			{
				if(state>0) state--;
			}
			memset(g_recv_data, 0xff, 14);
			break;
		}

		/*
		*  client recv
		*  data[0] = CMD_EXIT_GAME;
		*  data[1] = xP;
		*  data[2] = Addr;
		*/
		case CMD_EXIT_GAME:
		{
			exit_game_flag = 1;
			memset(g_recv_data, 0xff, 14);
			break;
		}

		default:	
			break;	
	}
}

static int elan_rx_thread(void *unused)
{
	printk("ELAN RX Monitor thread start!\n");
	
	while(1)
	{
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	
		if(task_run)
		{
		spin_lock(&elan_lock);
		read_fifo(g_recv_data, 14);
	
		__gpio_unmask_irq(GPIO_PKT_INT);
		
		//process cmd
		process_cmd();		
		spin_unlock(&elan_lock);
		}
		else
		{
			do_exit(0);
		}
	}
}

static irqreturn_t rx_irq_handle(int irq, void *dev_id)
{
	__gpio_ack_irq(GPIO_PKT_INT);
	__gpio_mask_irq(GPIO_PKT_INT);

	wake_up_process(elan_rx_task);
	return IRQ_HANDLED;
}

static void elan_hw_on()
{
	//close ELAN SOC
   // __gpio_as_func0(PIN_POWER_ELAN);
   // __gpio_enable_pull(PIN_POWER_ELAN);
   // __gpio_as_output(PIN_POWER_ELAN);
   // __gpio_set_pin(PIN_POWER_ELAN);

	printk("EM198850 Init...\n");
	unsigned char r_test;
	jz_spi_init();
	
	printk("After SPI Init...\n");
	
RF_INIT_AG:
	__gpio_as_output(GPIO_RESET);
	__gpio_clear_pin(GPIO_RESET);
	mdelay(10);
	__gpio_set_pin(GPIO_RESET);
	mdelay(10);

#if 1
	em198850_reg_init1();
	mdelay(10);
	rssi_init();
	mdelay(10);
	em198850_reg_init2();
	mdelay(10);
#endif

#if 0
	while(1)
	{
		unsigned char data;
		mdelay(10);
		em198850_reg_write(0x50, 0xAA);
		udelay(10);
		data = em198850_reg_read(0x50);
		printk("0x%x\n",data);
	}
#endif

	r_test = em198850_reg_test();
#if 1
	if(r_test == 0)
	{
		goto RF_INIT_AG;
	}
#endif

	printk("========  TEST OK ==========\n");
	memset(g_recv_data, 0xff, 14);
	rf_channel_set(35);
	rf_enter_rx_ack_crc_payload14();

	task_run = 1;
	elan_rx_task = kthread_run(elan_rx_thread, NULL, "elan_rx");
	if (IS_ERR(elan_rx_task)) {
		printk(KERN_ERR "elan: Failed to create system monitor thread.\n");
		return PTR_ERR(elan_rx_task);
	}

	__gpio_as_irq_rise_edge(GPIO_PKT_INT);
}

static void elan_hw_off()
{
	__gpio_clear_pin(GPIO_RESET);
	task_run = 0;
	wake_up_process(elan_rx_task);
	//kthread_stop(elan_rx_task);
	__gpio_as_input(GPIO_PKT_INT);

   machine_mode = 1;
   memset(g_recv_data, 0xff, 14); 
   max_clients = 0;
   state = 0;
   exit_game_flag = 0;
}


static void elan_hw_reset()
{
	unsigned char r_test;

	__gpio_mask_irq(GPIO_PKT_INT);

RF_INIT_AG1:	
		__gpio_as_output(GPIO_RESET);
		__gpio_clear_pin(GPIO_RESET);
		mdelay(5);
		__gpio_set_pin(GPIO_RESET);
		mdelay(5);

		em198850_reg_init1();
		mdelay(5);
		rssi_init();
		mdelay(5);
		em198850_reg_init2();
		mdelay(5);

	r_test = em198850_reg_test();
	if(r_test == 0)
	{
		goto RF_INIT_AG1;
	}

	memset(g_recv_data, 0xff, 14);
	rf_channel_set(35);
	rf_enter_rx_ack_crc_payload14();

	mdelay(10);	

	__gpio_unmask_irq(GPIO_PKT_INT);
	return;
}
//============================================================================
static int
elan_open(struct inode * inode, struct file * filp)
{
	return 0;
}

static int
elan_release(struct inode * inode, struct file * filp)
{
	return 0;
}

static int elan_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
		static elan_packet_t pkt;
		void __user *argp = (void __user *)arg;
		switch(cmd)
		{
#if 0
				//already in recv mode
				case ELAN_RECV_SEND:
				if (copy_from_user((void *)(&pkt), argp, sizeof(elan_packet_t)))
						return -EFAULT;
				wait_pkg_high();
				read_fifo(pkt.data_recv, pkt.len_recv);
				udelay(1);
				rf_enter_tx_ack_crc_payload4();
				write_fifo(pkt.data_send, pkt.len_send);
				wait_pkg_high();
		
				rf_enter_rx_ack_crc_payload4();
				
				if(copy_to_user(argp, (void *)(&pkt), sizeof(elan_packet_t)))
						return -EFAULT;
				//printk("pkt.data_recv:%x %x %x %x\n",pkt.data_recv[0], pkt.data_recv[1], pkt.data_recv[2], pkt.data_recv[3]);
				break;

				case ELAN_SEND_RECV:
				if (copy_from_user((void *)(&pkt), (void *)arg, sizeof(elan_packet_t)))
						return -EFAULT;

				write_fifo(pkt.data_send, pkt.len_send);
				wait_pkg_high();
				
				rf_enter_rx_ack_crc_payload4();
	
				wait_pkg_high();
				read_fifo(pkt.data_recv, pkt.len_recv);
		
				rf_enter_tx_ack_crc_payload4();
				if(copy_to_user(argp, (void *)(&pkt), sizeof(elan_packet_t)))
						return -EFAULT;
				//printk("pkt.data_recv:%x %x %x %x\n",pkt.data_recv[0], pkt.data_recv[1], pkt.data_recv[2], pkt.data_recv[3]);
				break;
#endif
				case ELAN_HW_ON:
				elan_hw_on();
				break;
		
				case ELAN_HW_OFF:
				elan_hw_off();
				break;	

				case ELAN_HW_RESET:
				elan_hw_reset();
				break;

				case ELAN_SET_MODE_HOST:
				machine_mode = 1;
				break;				

				case ELAN_SET_MODE_CLIENT:
				machine_mode = 0;
				break;				
	
				case ELAN_SET_MAX_CLIENTS:
				{
					if (copy_from_user((void *)(&max_clients), argp, sizeof(int)))
							return -EFAULT;
					break;
				}

				case ELAN_GET_HOST_STATE:
				{
						if(copy_to_user(argp, (void *)(&state), 4))
						{
								printk("copy to user error!\n");
								return -EFAULT;
						}			
						break;				
				}
		
	
				case ELAN_GET_CLIENT_STATE:
				{
						if(copy_to_user(argp, (void *)(&state), 4))
						{
								printk("copy to user error!\n");
								return -EFAULT;
						}			
						break;
				}

				case ELAN_SEND_VAL_BLK:
				{
						unsigned char send_data[14];

						if (copy_from_user((void *)(send_data), argp, 14))
								return -EFAULT;

						__gpio_as_input(GPIO_PKT_INT);
						rf_enter_tx_ack_crc_payload14();

						do{
								write_fifo(send_data, 14);
								wait_pkg_high();}
						while(lost_packet());

						rf_enter_rx_ack_crc_payload14();
						__gpio_as_irq_rise_edge(GPIO_PKT_INT);
						break;
				}
				case ELAN_SEND_VAL:
				{
						unsigned char send_data[14];
					
						if (copy_from_user((void *)(send_data), argp, 14))
								return -EFAULT;

						__gpio_as_input(GPIO_PKT_INT);
						rf_enter_tx_ack_crc_payload14();
						if(machine_mode == 1)	//host	
						{
								do{
								write_fifo(send_data, 14);
								wait_pkg_high();}
								while(lost_packet());
						}
						else  //client
						{
								write_fifo(send_data, 14);
								wait_pkg_high();
								if(error_flag)
								{
								printk("Kernel Error: %x %x %x %x\n",send_data[0],send_data[1],send_data[2],send_data[3]);
								}
#if 0
								//while(lost_packet())
								if(lost_packet())
								{
										udelay(2);
										//printk("client retransmit\n");
										write_fifo(send_data, 13);
										wait_pkg_high();
								}
#endif
						}

						rf_enter_rx_ack_crc_payload14();
						__gpio_as_irq_rise_edge(GPIO_PKT_INT);
						break;
				}
#if 0
				case ELAN_GET_VAL:
				{
						unsigned int frame_num;
						unsigned int keyval;
						int i;

						if (copy_from_user((void *)(&frame_num), argp, sizeof(int)))
								return -EFAULT;

						keyval = 0;
						for(i=0; i<KEY_CACHE_NUM; i++)
						{
								if((recv_keys[i]&0xFFFF0000) == ((frame_num&0xFFFF)<<16))
								{
										keyval = recv_keys[i]&0x7FFF;
										recv_keys[i] = 0;
										break;
								}
						}

						if(copy_to_user(argp, (void *)(&keyval), sizeof(int)))
								return -EFAULT;
						break;
				}
#endif
				case ELAN_CHECK_SYNC:
				{
						unsigned char frame_data[14] = {0xff};		
						spin_lock(&elan_lock);

						if(exit_game_flag)
						{
								frame_data[0] = 0xfe;

								if(copy_to_user(argp, (void *)(frame_data), 14))
								{
										printk("copy to user error!\n");
										return -EFAULT;
								}
								break;
						}					

						memcpy(frame_data, g_recv_data, 14);
						memset(g_recv_data, 0xff, 14);
						spin_unlock(&elan_lock);
#if 0
						if(frame_data[0]==g_recv_data[0] && frame_data[1]==g_recv_data[1] && 
							frame_data[2]==g_recv_data[2] && frame_data[3]==g_recv_data[3])
						{
							memcpy(frame_data, g_recv_data, 14);
							memset(g_recv_data, 0xff, 14);
						}
						else if(g_recv_data[0]==0xff && g_recv_data[1]==0xff && g_recv_data[2]==0xff && g_recv_data[3]==0xff)
						{
							frame_data[0] = 0xff;
							frame_data[1] = 0xff;
							frame_data[2] = 0xff;
							frame_data[3] = 0xff;
							memset(g_recv_data, 0xff, 14);
						}
						else if(g_recv_data[0]==0 && g_recv_data[1]==0 && g_recv_data[2]==0 && g_recv_data[3]==0)
						{
							error_flag = 1;
							printk("Kernel Error: %x %x %x %x, Wait %x %x %x %x\n",g_recv_data[0],g_recv_data[1],g_recv_data[2],g_recv_data[3],
									frame_data[0], frame_data[1], frame_data[2], frame_data[3]);
							memset(g_recv_data, 0xff, 14);
						}
						else
						{
							printk("Kernel Error: %x %x %x %x, Wait %x %x %x %x\n",g_recv_data[0],g_recv_data[1],g_recv_data[2],g_recv_data[3],
									frame_data[0], frame_data[1], frame_data[2], frame_data[3]);
								frame_data[0] = 0xff;
								frame_data[1] = 0xff;
								frame_data[2] = 0xff;
								frame_data[3] = 0xff;

								memset(g_recv_data, 0xff, 14);
						}
#endif
						if(copy_to_user(argp, (void *)(frame_data), 14))
						{
								printk("copy to user error!\n");
								return -EFAULT;
						}
						break;
				}

				//TEST IOCTL
				case ELAN_ENTER_TEST_RECV:
				{
					unsigned char frame_data[14] = {0};		
					__gpio_mask_irq(GPIO_PKT_INT);
					__gpio_as_input(GPIO_PKT_INT);
					rf_enter_rx_ack_crc_payload14();
					while(1)
					{
							wait_pkg_high();
							read_fifo(frame_data, 14);
							printk("%x %x %x\n",frame_data[0],frame_data[1],frame_data[2]);
					}
					break;
				}
	
				case ELAN_ENTER_TEST_SEND:
				{
					static int test_data = 0;
					unsigned char frame_data[14] = {0};		
					__gpio_mask_irq(GPIO_PKT_INT);
					__gpio_as_input(GPIO_PKT_INT);
					rf_enter_tx_ack_crc_payload14();
					frame_data[0] = 0xab;
					frame_data[1] = 0xcd;
					while(1)
					{
							frame_data[2] = test_data & 0xff;

							do{
									write_fifo(frame_data, 14);
									wait_pkg_high();}
							while(lost_packet());

							printk("%x %x %x\n",frame_data[1],frame_data[2],frame_data[3]);
							test_data++;
							udelay(1000 * 50);
					}
					break;
				}

		
				default:
				break;
		}
	
		return 0;
}

static struct file_operations elan_fops = {
	owner:          THIS_MODULE,
	ioctl:          elan_ioctl,
	open:			elan_open,
	release:		elan_release,
};

static struct miscdevice elan_dev = {
	minor:  ELAN_MINOR,
	name:   ELAN_NAME,
	fops:   &elan_fops,
};

// Driver Entry
static int __init em198850_init()
{
	int ret;
	
	if ((ret = misc_register(&elan_dev)) < 0) {
		printk("can't register misc device");
		return ret;
	}
	
	ret = request_irq(ELAN_PKT_IRQ, rx_irq_handle, IRQF_DISABLED, ELAN_NAME, NULL);
	if (ret) {
		printk("unable to get ELAN_PKT irq\n");
		return ret;
	}

	spin_lock_init(&elan_lock);

	//Test Send
#if 0
	unsigned int tt = 0;
	unsigned char send_data[5] = {0x0, 0x0, 0x0, 0x0, 0x0};
	rf_channel_set(23);
	rf_enter_tx_noack_nocrc_payload5();
	
	while(1)
	{
		send_data[0] = (tt++) & 0xFF;
		send_data[1] = (tt++) & 0xFF;
		send_data[2] = (tt++) & 0xFF;
		send_data[3] = (tt++) & 0xFF;
		send_data[4] = (tt++) & 0xFF;
		write_fifo(send_data, 5);
		wait_pkg_high();
		mdelay(500);
	}
#endif

	//client_test();
	//host_test();	

#if 0
	//Test Recv
	unsigned char send_data[5] = {0x0, 0x0, 0x0, 0x0, 0x0};
	rf_channel_set(23);
	rf_enter_rx_noack_nocrc_payload5();
	
	while(1)
	{
		send_data[0] = 0;
		send_data[1] = 0;
		send_data[2] = 0;
		send_data[3] = 0;
		send_data[4] = 0;
		wait_pkg_high();
		read_fifo(send_data, 5);
		//mdelay(500);
		printk("0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",send_data[0], send_data[1], send_data[2], 
				send_data[3], send_data[4]);
	}
#endif

	return 0;
}


static void __exit em198850_exit()
{
	jz_spi_deinit();
	misc_deregister(&elan_dev);
}


module_init(em198850_init);
module_exit(em198850_exit);
