#include "fsusb.h"
#include "ch32v003fun.h"
#include <string.h>

uint32_t USBDEBUG0, USBDEBUG1, USBDEBUG2;


#define UEP_CTRL_H(n) (((uint16_t*)&USBFS->UEP0_CTRL_H)[n*2])

struct _USBState FSUSBCTX;

// based on https://github.com/openwch/ch32x035/tree/main/EVT/EXAM/USB/USBFS/DEVICE/CompositeKM/User

// Mask for the combined USBFSD->INT_FG + USBFSD->INT_ST
#define CRB_U_IS_NAK     (1<<7)
#define CTOG_MATCH_SYNC  (1<<6)
#define CRB_U_SIE_FREE   (1<<5)
#define CRB_UIF_FIFO_OV  (1<<4)
#define CRB_UIF_HST_SOF  (1<<3)
#define CRB_UIF_SUSPEND  (1<<2)
#define CRB_UIF_TRANSFER (1<<1)
#define CRB_UIF_BUS_RST  (1<<0)
#define CSETUP_ACT	     (1<<15)
#define CRB_UIS_TOG_OK   (1<<14)
#define CMASK_UIS_TOKEN  (3<<12)
#define CMASK_UIS_ENDP   (0xf<<8)

#define CUIS_TOKEN_OUT	 0x0
#define CUIS_TOKEN_SOF   0x1
#define CUIS_TOKEN_IN    0x2
#define CUIS_TOKEN_SETUP 0x3

#if 0
static inline void fastcopy( uint8_t * dest, const uint8_t * src, int len )
{
	src = ((intptr_t)src) & ~3;
	asm volatile( "\
		add a3, %[src], %[len]\n\
1:\n\
		lw a4, 0(%[src])\n\
		lw a5, 4(%[src])\n\
		lw s1, 8(%[src])\n\
		lw s2, 12(%[src])\n\
		addi %[src],%[src],16\n\
		sw a4, 0(%[dest])\n\
		sw a5, 4(%[dest])\n\
		sw s1, 8(%[dest])\n\
		sw s2, 12(%[dest])\n\
		addi %[dest],%[dest],16\n\
		bgtu a3,%[src],1b\n\
	" : [dest]"+r"(dest), [src]"+r"(src) : [len]"r"(len) : "memory", "a3", "a4", "a5", "s1", "s2" );
}
#else
static inline void fastcopy( uint8_t * dest, const uint8_t * src, int len )
{
	DMA1_Channel7->CFGR = 0;
	DMA1_Channel7->MADDR = (uintptr_t)src;
	DMA1_Channel7->PADDR = (uintptr_t)dest;
	DMA1_Channel7->CNTR  = (len+3)/4;
	DMA1_Channel7->CFGR  =
		DMA_M2M_Enable | 
		DMA_DIR_PeripheralDST |
		DMA_Priority_Low |
		DMA_MemoryDataSize_Word |
		DMA_PeripheralDataSize_Word |
		DMA_MemoryInc_Enable |
		DMA_PeripheralInc_Enable |
		DMA_Mode_Normal | DMA_CFGR1_EN;
	//XXX TODO: Somehow, it seems to work (unsafely) without this.
#if !( FUSB_CURSED_TURBO_DMA == 1 )
	while( DMA1_Channel7->CNTR );
#endif

}
#endif

void USBFS_IRQHandler() __attribute__((section(".text.vector_handler")))  __attribute__((interrupt));

void USBFS_InternalFinishSetup();

void USBFS_IRQHandler()
{
	// Based on https://github.com/openwch/ch32x035/blob/main/EVT/EXAM/USB/USBFS/DEVICE/CompositeKM/User/ch32x035_usbfs_device.c
	// Combined FG + ST flag.
	uint16_t intfgst = *(uint16_t*)(&USBFS->INT_FG);
	int len = 0;
	struct _USBState * ctx = &FSUSBCTX;
	GPIOA->BSHR = 1;

	// TODO: Check if needs to be do-while to re-check.
	if( intfgst & CRB_UIF_TRANSFER )
	{
		int token = ( intfgst & CMASK_UIS_TOKEN) >> 12;
		int ep = ( intfgst & CMASK_UIS_ENDP ) >> 8;

		switch ( token )
		{
		case CUIS_TOKEN_IN:
			if( ep )
			{
				if( ep < FUSB_CONFIG_EPS )
				{
					UEP_CTRL_H(ep) = ( UEP_CTRL_H(ep) & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
					UEP_CTRL_H(ep) ^= USBFS_UEP_T_TOG;
					ctx->USBFS_Endp_Busy[ ep ] = 0;
				}
			}
			else
			{
				/* end-point 0 data in interrupt */
				if( ctx->USBFS_SetupReqLen == 0 )
				{
					USBFS->UEP0_CTRL_H = USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
				}

				if ( ( ctx->USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
				{
					/* Non-standard request endpoint 0 Data upload, noted by official docs, but I don't think this would ever really be used. */
				}
				else
				{
					switch( FSUSBCTX.USBFS_SetupReqCode )
					{
						case USB_GET_DESCRIPTOR:
							len = ctx->USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE ? DEF_USBD_UEP0_SIZE : ctx->USBFS_SetupReqLen;
							//memcpy( CTRL0BUFF, ctx->pUSBFS_Descr, len ); // FYI -> IS IT POSSIBLE TO DO THIS WITH DMA????
							fastcopy( CTRL0BUFF, ctx->pUSBFS_Descr, len ); // FYI -> Would need to do this if using DMA
							USBFS->UEP0_TX_LEN = len;
							USBFS->UEP0_CTRL_H ^= USBFS_UEP_T_TOG;
							ctx->USBFS_SetupReqLen -= len;
							ctx->pUSBFS_Descr += len;
							break;

						case USB_SET_ADDRESS:
							USBFS->DEV_ADDR = ( USBFS->DEV_ADDR & USBFS_UDA_GP_BIT ) | ctx->USBFS_DevAddr;
							break;

						default:
							break;
					}
				}
			}
			break;

		/* data-out stage processing */
		case CUIS_TOKEN_OUT:
			switch( ep )
			{
				/* end-point 0 data out interrupt */
				case DEF_UEP0:
					if( intfgst & CRB_UIS_TOG_OK )
					{
						if( ( FSUSBCTX.USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
						{
							if( ( FSUSBCTX.USBFS_SetupReqType & USB_REQ_TYP_MASK ) == USB_REQ_TYP_CLASS )
							{
								switch( FSUSBCTX.USBFS_SetupReqCode )
								{
									case HID_SET_REPORT:
										//KB_LED_Cur_Status = USBFS_EP0_Buf[ 0 ];
										FSUSBCTX.USBFS_SetupReqLen = 0;
										break;
									default:
										break;
								}
							}
						}
						else
						{
							/* Standard request end-point 0 Data download */
							/* Add your code here */
						}
					}
					if( FSUSBCTX.USBFS_SetupReqLen == 0 )
					{
						USBFS->UEP0_TX_LEN  = 0;
						USBFS->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
					}
					break;

				default:
					break;
			}
			break;

		/* Setup stage processing */
		case CUIS_TOKEN_SETUP:
			USBFS->UEP0_CTRL_H = USBFS_UEP_T_TOG|USBFS_UEP_T_RES_NAK|USBFS_UEP_R_TOG|USBFS_UEP_R_RES_NAK;

			/* Store All Setup Values */
			int USBFS_SetupReqType = FSUSBCTX.USBFS_SetupReqType  = pUSBFS_SetupReqPak->bmRequestType;
			int USBFS_SetupReqCode = FSUSBCTX.USBFS_SetupReqCode  = pUSBFS_SetupReqPak->bRequest;
			int USBFS_SetupReqLen = FSUSBCTX.USBFS_SetupReqLen   = pUSBFS_SetupReqPak->wLength;
			int USBFS_SetupReqIndex = pUSBFS_SetupReqPak->wIndex;
			int USBFS_IndexValue = FSUSBCTX.USBFS_IndexValue = ( pUSBFS_SetupReqPak->wIndex << 16 ) | pUSBFS_SetupReqPak->wValue;
			len = 0;

			if( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
			{
#if FUSB_HID_INTERFACES > 0 
				if( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) == USB_REQ_TYP_CLASS )
				{
					/* Class Request */
					switch( USBFS_SetupReqCode )
					{
						case HID_SET_REPORT:
							break;

						case HID_SET_IDLE:
							if( USBFS_SetupReqIndex < FUSB_HID_INTERFACES )
								FSUSBCTX.USBFS_HidIdle[ USBFS_SetupReqIndex ] = (uint8_t)( USBFS_IndexValue >> 8 );
							break;
						case HID_SET_PROTOCOL:
							if ( USBFS_SetupReqIndex < FUSB_HID_INTERFACES )
								FSUSBCTX.USBFS_HidProtocol[USBFS_SetupReqIndex] = (uint8_t)USBFS_IndexValue;
							break;

						case HID_GET_IDLE:
							if( USBFS_SetupReqIndex < FUSB_HID_INTERFACES )
							{
								CTRL0BUFF[0] = FSUSBCTX.USBFS_HidIdle[ USBFS_SetupReqIndex ];
								len = 1;
							}
							break;

						case HID_GET_PROTOCOL:
							if( USBFS_SetupReqIndex < FUSB_HID_INTERFACES )
							{
								CTRL0BUFF[0] = FSUSBCTX.USBFS_HidProtocol[ USBFS_SetupReqIndex ];
								len = 1;
							}
							break;

						default:
							goto sendstall;
							break;
					}
				}
#endif
			}
			else
			{
				/* usb standard request processing */
				switch( USBFS_SetupReqCode )
				{
					/* get device/configuration/string/report/... descriptors */
					case USB_GET_DESCRIPTOR:
					{
						const struct descriptor_list_struct * e = descriptor_list;
						const struct descriptor_list_struct * e_end = e + DESCRIPTOR_LIST_ENTRIES;
						for( ; e != e_end; e++ )
						{
							if( e->lIndexValue == USBFS_IndexValue )
							{
								ctx->pUSBFS_Descr = e->addr;
								len = e->length;
								break;
							}
						}
						if( e == e_end )
						{
							goto sendstall;
						}

						/* Copy Descriptors to Endp0 DMA buffer */
						if( USBFS_SetupReqLen > len )
						{
							USBFS_SetupReqLen = len;
						}
						len = ( USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE ) ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
						fastcopy( CTRL0BUFF, ctx->pUSBFS_Descr, len ); //memcpy( CTRL0BUFF, ctx->pUSBFS_Descr, len );
						USBFS->UEP0_TX_LEN = len;
						USBFS->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
						ctx->pUSBFS_Descr += len;
						goto replycomplete;
						break;
					}

					/* Set usb address */
					case USB_SET_ADDRESS:
						ctx->USBFS_DevAddr = (uint8_t)( ctx->USBFS_IndexValue & 0xFF );
						break;

					/* Get usb configuration now set */
					case USB_GET_CONFIGURATION:
						CTRL0BUFF[0] = ctx->USBFS_DevConfig;
						if( ctx->USBFS_SetupReqLen > 1 )
							ctx->USBFS_SetupReqLen = 1;
						break;

					/* Set usb configuration to use */
					case USB_SET_CONFIGURATION:
						ctx->USBFS_DevConfig = (uint8_t)( ctx->USBFS_IndexValue & 0xFF );
						ctx->USBFS_DevEnumStatus = 0x01;
						break;

					/* Clear or disable one usb feature */
					case USB_CLEAR_FEATURE:
#if FUSB_SUPPORTS_SLEEP
						if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
						{
							/* clear one device feature */
							if( (uint8_t)( USBFS_IndexValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
							{
								/* clear usb sleep status, device not prepare to sleep */
								ctx->USBFS_DevSleepStatus &= ~0x01;
							}
							else
							{
								goto sendstall;
							}
						}
						else
#endif
						if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
						{
							if( (uint8_t)( USBFS_IndexValue & 0xFF ) == USB_REQ_FEAT_ENDP_HALT )
							{
								/* Clear End-point Feature */
								if( ( USBFS_SetupReqIndex & DEF_UEP_IN ) && ep < FUSB_CONFIG_EPS ) 
								{
									UEP_CTRL_H(ep) = USBFS_UEP_T_RES_NAK;
								}
								else
								{
									goto sendstall;
								}
							}
							else
							{
								goto sendstall;
							}
						}
						else
						{
							goto sendstall;
						}
						break;

					/* set or enable one usb feature */
					case USB_SET_FEATURE:
						if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
						{
#if FUSB_SUPPORTS_SLEEP
							/* Set Device Feature */
							if( (uint8_t)( USBFS_IndexValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
							{
								/* Set Wake-up flag, device prepare to sleep */
								USBFS_DevSleepStatus |= 0x01;
							}
							else
#endif
							{
								goto sendstall;
							}
						}
						else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
						{
							/* Set Endpoint Feature */
							if( (uint8_t)( USBFS_IndexValue & 0xFF ) == USB_REQ_FEAT_ENDP_HALT )
							{
								if( ( USBFS_SetupReqIndex & DEF_UEP_IN ) && ep < FUSB_CONFIG_EPS )
									UEP_CTRL_H(ep) = ( UEP_CTRL_H(ep) & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
							}
							else
								goto sendstall;
						}
						else
							goto sendstall;
						break;

					/* This request allows the host to select another setting for the specified interface  */
					case USB_GET_INTERFACE:
						CTRL0BUFF[0] = 0x00;
						if( USBFS_SetupReqLen > 1 ) USBFS_SetupReqLen = 1;
						break;

					case USB_SET_INTERFACE:
						break;

					/* host get status of specified device/interface/end-points */
					case USB_GET_STATUS:
						CTRL0BUFF[0] = 0x00;
						CTRL0BUFF[1] = 0x00;
						if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
						{
#if FUSB_SUPPORTS_SLEEP
							CTRL0BUFF[0] = (ctx->USBFS_DevSleepStatus & 0x01)<<1;
#else
							CTRL0BUFF[0] = 0x00;
#endif
						}
						else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
						{
							if( ( USBFS_SetupReqIndex & DEF_UEP_IN ) && ep < FUSB_CONFIG_EPS )
								CTRL0BUFF[0] = ( UEP_CTRL_H(ep) & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL;
							else
								goto sendstall;
						}
						else
							goto sendstall;
						if( USBFS_SetupReqLen > 2 )
							USBFS_SetupReqLen = 2;
						break;

					default:
						goto sendstall;
						break;
				}
			}


			{
				/* end-point 0 data Tx/Rx */
				if( USBFS_SetupReqType & DEF_UEP_IN )
				{
					len = ( USBFS_SetupReqLen > DEF_USBD_UEP0_SIZE )? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
					USBFS_SetupReqLen -= len;
					USBFS->UEP0_TX_LEN = len;
					USBFS->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
				}
				else
				{
					if( USBFS_SetupReqLen == 0 )
					{
						USBFS->UEP0_TX_LEN = 0;
						USBFS->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
					}
					else
					{
						USBFS->UEP0_CTRL_H = USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
					}
				}
			}
			break;

			// This might look a little weird, for error handling but it saves a nontrivial amount of storage, and simplifies
			// control flow to hard-abort here.
		sendstall:
			// if one request not support, return stall.  Stall means permanent error.
			USBFS->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_STALL|USBFS_UEP_R_TOG | USBFS_UEP_R_RES_STALL;
		replycomplete:
			break;

		/* Sof pack processing */
		case CUIS_TOKEN_SOF:
			break;

		default :
			break;
		}


	}
	else if( intfgst & CRB_UIF_BUS_RST )
	{
		/* usb reset interrupt processing */
		ctx->USBFS_DevConfig = 0;
		ctx->USBFS_DevAddr = 0;
		ctx->USBFS_DevSleepStatus = 0;
		ctx->USBFS_DevEnumStatus = 0;

		USBFS->DEV_ADDR = 0;
		USBFS_InternalFinishSetup( );
	}
	else if( intfgst & CRB_UIF_SUSPEND )
	{
		/* usb suspend interrupt processing */
		if( USBFS->MIS_ST & USBFS_UMS_SUSPEND )
		{
			ctx->USBFS_DevSleepStatus |= 0x02;
			if( ctx->USBFS_DevSleepStatus == 0x03 )
			{
				/* Handling usb sleep here */
				//TODO: MCU_Sleep_Wakeup_Operate( );
			}
		}
		else
		{
			ctx->USBFS_DevSleepStatus &= ~0x02;
		}
	}

	// Handle any other interrupts and just clear them out.
	*(uint16_t*)(&USBFS->INT_FG) = intfgst;

	//intfgst = *(uint16_t*)(&USBFS->INT_FG);
	GPIOA->BSHR = 1<<16;
}

void USBFS_InternalFinishSetup()
{
    USBFS->UEP4_1_MOD = RB_UEP1_TX_EN;
    USBFS->UEP2_3_MOD = RB_UEP2_TX_EN;
	USBFS->UEP567_MOD = 0;

	// This is really cursed.  Somehow it doesn't explode.
	// But, normally the USB wants a separate buffer here.
	USBFS->UEP0_DMA = (uintptr_t)CTRL0BUFF;

	UEP_CTRL_H(0) = USBFS_UEP_R_RES_ACK | USBFS_UEP_T_RES_NAK;
	int i;
	for( i = 1; i < FUSB_CONFIG_EPS; i++ )
		UEP_CTRL_H(i) = USBFS_UEP_T_RES_NAK;
                                                                    
    for(uint8_t i=0; i< sizeof(FSUSBCTX.USBFS_Endp_Busy)/sizeof(FSUSBCTX.USBFS_Endp_Busy[0]); i++ )
    {
        FSUSBCTX.USBFS_Endp_Busy[ i ] = 0;
    }
}

int FSUSBSetup()
{
	RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOC;
	RCC->AHBPCENR |= RCC_USBFS | RCC_AHBPeriph_DMA1;

	NVIC_EnableIRQ( USBFS_IRQn );

	AFIO->CTLR |= USB_PHY_V33;

	USBFS->BASE_CTRL = RB_UC_RESET_SIE | RB_UC_CLR_ALL;
	USBFS->BASE_CTRL = 0x00;

	USBFS_InternalFinishSetup();

	// Enter device mode.
	USBFS->INT_EN = RB_UIE_SUSPEND | RB_UIE_TRANSFER | RB_UIE_BUS_RST;
	USBFS->DEV_ADDR = 0x00;
	USBFS->BASE_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;
	USBFS->INT_FG = 0xff;
	USBFS->UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;

	// Go on-bus.

	// From the TRM:
	//
	//   USB multiplexing IO pins enable:
	//
	// Enabling USB requires, in addition to USB_IOEN set to
	// 1, the setting of: MODE=0 in GPIO configuration register
	// GPIOC_CFGXR corresponding to PC16 and PC17 to
	// select the input mode.
	//
	// for USB device, CNF=10 corresponding to PC17 to select
	// the input mode with up and down For USB devices, PC17
	// corresponding to CNF=10 selects the input mode with up
	// and down pull, PC17 corresponding to bit 1 in
	// GPIOC_OUTDR selects the up pull, and PC16
	// corresponding to CNF=01 selects the floating input.

#if FUSB_5V_OPERATION
	// XXX This is dubious, copied from x035 - checkme (UDP_PUE_10K)
	AFIO->CTLR = (AFIO->CTLR & ~(UDP_PUE_MASK | UDM_PUE_MASK | USB_PHY_V33)) | UDP_PUE_10K | USB_IOEN;
#else
	AFIO->CTLR = (AFIO->CTLR & ~(UDP_PUE_MASK | UDM_PUE_MASK )) | USB_PHY_V33 | UDP_PUE_1K5 | USB_IOEN;
#endif

	AFIO->CTLR = (AFIO->CTLR & ~(UDP_PUE_11 | UDM_PUE_11 )) | USB_PHY_V33 | USB_IOEN | UDP_PUE_11; //1.5k pullup

	// Enable PC16/17 Alternate Function (USB)
	// According to EVT, GPIO16 = GPIO_Mode_IN_FLOATING, GPIO17 = GPIO_Mode_IPU
	GPIOC->CFGXR = 	( GPIOC->CFGXR & ~( (0xf<<(4*0)) | (0xf<<(4*1)) ) )  |
					(((GPIO_CFGLR_IN_FLOAT)<<(4*0)) | (((GPIO_CFGLR_IN_PUPD)<<(4*1)))); // MSBs are CNF, LSBs are MODE
	GPIOC->BSXR = 1<<1; // PC17 on.

	// Go on-bus.
	return 0;
}



