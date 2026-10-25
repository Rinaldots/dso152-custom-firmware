/*
 * usb_cdc.h — porta serial virtual (USB CDC-ACM) minima, por polling.
 */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

void usb_cdc_init(void);        /* precisa do clock em 72 MHz (USB a 48 MHz) */
void usb_cdc_poll(void);        /* chamar sempre no loop principal */

int  usb_cdc_configured(void);  /* enumerado pelo host */
int  usb_cdc_ready(void);       /* enumerado e com a porta aberta (DTR) */

/* Envio assincrono: o buffer precisa continuar valido ate tx_busy() == 0. */
void usb_cdc_write(const uint8_t *data, uint32_t len);
int  usb_cdc_tx_busy(void);

int  usb_cdc_getc(void);        /* proximo byte recebido, ou -1 */

#endif
