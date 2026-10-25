/*
 * usb_cdc.c — USB CDC-ACM minimo para o USBD do CH32F103 (igual ao do
 * STM32F103), tudo por polling, sem biblioteca.
 *
 * Endpoints:
 *   EP0  controle, 64 bytes
 *   EP1  bulk OUT/IN, 64 bytes (dados da serial)
 *   EP2  interrupt IN, 16 bytes (notificacoes CDC; nunca usado, mas exigido)
 *
 * O descritor usa IAD (classe 0xEF/02/01), assim o Windows 10/11 carrega o
 * usbser.sys sozinho, sem .inf. VID/PID 1209:0001 = PID de teste do pid.codes.
 */
#include <stdint.h>
#include "ch32f103.h"
#include "usb_cdc.h"

void delay_ms(uint32_t ms);

#define EP0_SIZE   64u
#define DATA_SIZE  64u

/* ---------------------- memoria de pacotes (PMA) ---------------------- */
/* tabela de buffers no offset 0; cada EP usa 8 bytes dela */
#define PMA_EP0_RX  0x040u
#define PMA_EP0_TX  0x080u
#define PMA_EP1_RX  0x0C0u
#define PMA_EP1_TX  0x100u
#define PMA_EP2_TX  0x140u
#define COUNT_RX_64 0x8400u   /* BL_SIZE=1 (blocos de 32), NUM_BLOCK=1 -> 64 */

static volatile uint16_t *pma16(uint32_t off)
{
    return (volatile uint16_t *)(USB_PMA_BASE + 2u * off);
}
#define BT_ADDR_TX(ep)  (*pma16(8u * (ep) + 0u))
#define BT_COUNT_TX(ep) (*pma16(8u * (ep) + 2u))
#define BT_ADDR_RX(ep)  (*pma16(8u * (ep) + 4u))
#define BT_COUNT_RX(ep) (*pma16(8u * (ep) + 6u))

static void pma_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    volatile uint16_t *p = pma16(off);
    for (uint32_t i = 0; i < n; i += 2u) {
        uint16_t w = src[i];
        if (i + 1u < n) w |= (uint16_t)(src[i + 1u] << 8);
        *p = w;
        p += 2;                       /* meia-palavra a cada 4 bytes */
    }
}

static void pma_read(uint32_t off, uint8_t *dst, uint32_t n)
{
    volatile uint16_t *p = pma16(off);
    for (uint32_t i = 0; i < n; i += 2u) {
        uint16_t w = *p;
        p += 2;
        dst[i] = (uint8_t)w;
        if (i + 1u < n) dst[i + 1u] = (uint8_t)(w >> 8);
    }
}

/* ------------------------- registradores EPnR ------------------------- */
#define EP_CTR_RX   0x8000u   /* rc_w0 */
#define EP_DTOG_RX  0x4000u   /* toggle */
#define EP_STAT_RX  0x3000u   /* toggle */
#define EP_SETUP    0x0800u
#define EP_TYPE     0x0600u
#define EP_KIND     0x0100u
#define EP_CTR_TX   0x0080u   /* rc_w0 */
#define EP_DTOG_TX  0x0040u   /* toggle */
#define EP_STAT_TX  0x0030u   /* toggle */
#define EP_EA       0x000Fu
#define EP_KEEP     (EP_TYPE | EP_KIND | EP_EA)   /* bits que se reescrevem iguais */

#define EP_BULK      0x0000u
#define EP_CONTROL   0x0200u
#define EP_INTERRUPT 0x0600u

#define RX_STALL 0x1000u
#define RX_NAK   0x2000u
#define RX_VALID 0x3000u
#define TX_STALL 0x0010u
#define TX_NAK   0x0020u
#define TX_VALID 0x0030u

/* Escrever 1 num CTR preserva, 0 limpa; escrever 1 num bit toggle inverte. */
static void ep_stat_tx(uint32_t ep, uint16_t stat)
{
    uint16_t r = (uint16_t)USB_EPR(ep);
    USB_EPR(ep) = (r & EP_KEEP) | EP_CTR_RX | EP_CTR_TX | ((r ^ stat) & EP_STAT_TX);
}

static void ep_stat_rx(uint32_t ep, uint16_t stat)
{
    uint16_t r = (uint16_t)USB_EPR(ep);
    USB_EPR(ep) = (r & EP_KEEP) | EP_CTR_RX | EP_CTR_TX | ((r ^ stat) & EP_STAT_RX);
}

static void ep_clear_ctr_rx(uint32_t ep)
{
    uint16_t r = (uint16_t)USB_EPR(ep);
    USB_EPR(ep) = (r & EP_KEEP) | EP_CTR_TX;
}

static void ep_clear_ctr_tx(uint32_t ep)
{
    uint16_t r = (uint16_t)USB_EPR(ep);
    USB_EPR(ep) = (r & EP_KEEP) | EP_CTR_RX;
}

/* zera o DTOG de uma direcao (CLEAR_FEATURE ENDPOINT_HALT) */
static void ep_clear_dtog(uint32_t ep, int in)
{
    uint16_t r = (uint16_t)USB_EPR(ep);
    USB_EPR(ep) = (r & EP_KEEP) | EP_CTR_RX | EP_CTR_TX | (r & (in ? EP_DTOG_TX : EP_DTOG_RX));
}

/* tipo + endereco, CTRs e DTOGs zerados, STAT_RX/STAT_TX definidos */
static void ep_setup(uint32_t ep, uint16_t type, uint16_t stat_rx, uint16_t stat_tx)
{
    uint16_t r = (uint16_t)USB_EPR(ep);
    USB_EPR(ep) = type | ep
                | ((r ^ (stat_rx | stat_tx)) & (EP_DTOG_RX | EP_STAT_RX | EP_DTOG_TX | EP_STAT_TX));
}

/* ---------------------------- descritores ----------------------------- */
static const uint8_t DEV_DESC[18] = {
    18, 0x01,             /* bLength, DEVICE */
    0x00, 0x02,           /* USB 2.0 */
    0xEF, 0x02, 0x01,     /* misc / IAD */
    EP0_SIZE,
    0x09, 0x12,           /* VID 0x1209 (pid.codes) */
    0x01, 0x00,           /* PID 0x0001 (teste) */
    0x00, 0x01,           /* bcdDevice 1.00 */
    1, 2, 3,              /* strings: fabricante, produto, serie */
    1,                    /* 1 configuracao */
};

static const uint8_t CFG_DESC[75] = {
    /* configuracao */
    9, 0x02, 75, 0, 2, 1, 0, 0x80, 50,     /* 2 interfaces, 100 mA */
    /* IAD: interfaces 0..1 = funcao CDC ACM */
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,
    /* interface 0: comunicacao CDC ACM, 1 endpoint */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,             /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,                /* call management */
    4, 0x24, 0x02, 0x02,                   /* ACM: line coding + control line state */
    5, 0x24, 0x06, 0, 1,                   /* union: master 0, slave 1 */
    7, 0x05, 0x82, 0x03, 16, 0, 255,       /* EP2 IN interrupt */
    /* interface 1: dados CDC, 2 endpoints */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, 0x01, 0x02, DATA_SIZE, 0, 0,  /* EP1 OUT bulk */
    7, 0x05, 0x81, 0x02, DATA_SIZE, 0, 0,  /* EP1 IN bulk */
};

static const char *const STRINGS[] = { 0, "DSO152 hack", "DSO152 scope (CDC)", "0001" };

/* ------------------------------ estado -------------------------------- */
static uint8_t  ep0_buf[EP0_SIZE];     /* SETUP / dados OUT do EP0 */
static uint8_t  scratch[64];           /* respostas montadas na hora */
static uint8_t  line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };  /* 115200 8N1 */

static const uint8_t *ep0_ptr;         /* estagio de dados IN do EP0 */
static uint32_t ep0_left;
static uint8_t  ep0_zlp, ep0_in;
static uint8_t  pending_addr;          /* SET_ADDRESS vale depois do status */
static uint8_t  pending_out;           /* request de classe esperando dados OUT */
static uint8_t  configured, dtr;

static const uint8_t *tx_ptr;          /* envio no EP1 IN */
static uint32_t tx_left;
static uint8_t  tx_active, tx_zlp;

#define RX_RING 128u
static uint8_t  rx_ring[RX_RING];
static uint32_t rx_head, rx_tail;

/* ------------------------------- EP0 ---------------------------------- */
static void ep0_stall(void)
{
    ep_stat_tx(0, TX_STALL);
    ep_stat_rx(0, RX_STALL);
}

static void ep0_status_in(void)
{
    BT_COUNT_TX(0) = 0;
    ep_stat_tx(0, TX_VALID);
    ep_stat_rx(0, RX_VALID);
}

static void ep0_in_packet(void)
{
    uint32_t n = ep0_left > EP0_SIZE ? EP0_SIZE : ep0_left;
    pma_write(PMA_EP0_TX, ep0_ptr, n);
    BT_COUNT_TX(0) = (uint16_t)n;
    ep0_ptr += n;
    ep0_left -= n;
    ep_stat_tx(0, TX_VALID);
}

static void ep0_in_start(const uint8_t *data, uint32_t len, uint32_t wlength)
{
    if (len > wlength) len = wlength;
    ep0_ptr = data;
    ep0_left = len;
    ep0_zlp = (len < wlength) && ((len % EP0_SIZE) == 0u);
    ep0_in = 1;
    ep0_in_packet();
    ep_stat_rx(0, RX_VALID);          /* status OUT do host pode vir a qualquer hora */
}

static uint32_t string_desc(uint32_t idx)
{
    if (idx == 0u) {
        scratch[0] = 4; scratch[1] = 0x03; scratch[2] = 0x09; scratch[3] = 0x04;  /* en-US */
        return 4;
    }
    if (idx >= sizeof STRINGS / sizeof STRINGS[0]) return 0;
    const char *s = STRINGS[idx];
    uint32_t n = 2;
    while (*s && n + 2u <= sizeof scratch) {
        scratch[n++] = (uint8_t)*s++;
        scratch[n++] = 0;
    }
    scratch[0] = (uint8_t)n;
    scratch[1] = 0x03;
    return n;
}

static void set_configuration(uint32_t cfg)
{
    configured = (cfg == 1u);
    dtr = 0;
    tx_active = 0;
    if (configured) {
        ep_setup(1, EP_BULK, RX_VALID, TX_NAK);
        ep_setup(2, EP_INTERRUPT, 0, TX_NAK);
    } else {
        ep_setup(1, EP_BULK, 0, 0);
        ep_setup(2, EP_INTERRUPT, 0, 0);
    }
}

static void handle_setup(void)
{
    uint8_t  type    = ep0_buf[0];
    uint8_t  request = ep0_buf[1];
    uint16_t value   = (uint16_t)(ep0_buf[2] | (ep0_buf[3] << 8));
    uint16_t index   = (uint16_t)(ep0_buf[4] | (ep0_buf[5] << 8));
    uint16_t length  = (uint16_t)(ep0_buf[6] | (ep0_buf[7] << 8));

    if ((type & 0x60u) == 0x00u) {                 /* padrao */
        switch (request) {
        case 0x06: {                               /* GET_DESCRIPTOR */
            uint32_t n;
            switch (value >> 8) {
            case 1: ep0_in_start(DEV_DESC, sizeof DEV_DESC, length); return;
            case 2: ep0_in_start(CFG_DESC, sizeof CFG_DESC, length); return;
            case 3:
                n = string_desc(value & 0xFFu);
                if (n) { ep0_in_start(scratch, n, length); return; }
                break;
            default: break;                        /* qualifier etc.: STALL */
            }
            break;
        }
        case 0x05:                                 /* SET_ADDRESS */
            pending_addr = (uint8_t)(value & 0x7Fu);
            ep0_status_in();
            return;
        case 0x09:                                 /* SET_CONFIGURATION */
            set_configuration(value & 0xFFu);
            ep0_status_in();
            return;
        case 0x08:                                 /* GET_CONFIGURATION */
            scratch[0] = configured;
            ep0_in_start(scratch, 1, length);
            return;
        case 0x00:                                 /* GET_STATUS */
            scratch[0] = 0; scratch[1] = 0;
            ep0_in_start(scratch, 2, length);
            return;
        case 0x0A:                                 /* GET_INTERFACE */
            scratch[0] = 0;
            ep0_in_start(scratch, 1, length);
            return;
        case 0x01:                                 /* CLEAR_FEATURE */
            if ((type & 0x1Fu) == 0x02u && value == 0u) {    /* ENDPOINT_HALT */
                uint32_t ep = index & 0x0Fu;
                if (ep == 1u || ep == 2u) ep_clear_dtog(ep, (index & 0x80u) != 0u);
            }
            ep0_status_in();
            return;
        case 0x03:                                 /* SET_FEATURE */
        case 0x0B:                                 /* SET_INTERFACE */
            ep0_status_in();
            return;
        default:
            break;
        }
    } else if ((type & 0x60u) == 0x20u) {          /* classe CDC */
        switch (request) {
        case 0x20:                                 /* SET_LINE_CODING: 7 bytes OUT */
            pending_out = request;
            ep_stat_rx(0, RX_VALID);
            return;
        case 0x21:                                 /* GET_LINE_CODING */
            ep0_in_start(line_coding, sizeof line_coding, length);
            return;
        case 0x22:                                 /* SET_CONTROL_LINE_STATE */
            dtr = (uint8_t)(value & 1u);
            ep0_status_in();
            return;
        case 0x23:                                 /* SEND_BREAK */
            ep0_status_in();
            return;
        default:
            break;
        }
    }
    ep0_stall();
}

static void ep0_rx(void)
{
    uint16_t r = (uint16_t)USB_EPR(0);
    uint32_t n = BT_COUNT_RX(0) & 0x3FFu;
    if (n > EP0_SIZE) n = EP0_SIZE;

    if (r & EP_SETUP) {
        pma_read(PMA_EP0_RX, ep0_buf, 8);
        ep_clear_ctr_rx(0);
        ep0_in = 0;
        pending_out = 0;
        handle_setup();
        return;
    }

    pma_read(PMA_EP0_RX, ep0_buf, n);
    ep_clear_ctr_rx(0);
    if (pending_out == 0x20u) {                    /* dados do SET_LINE_CODING */
        for (uint32_t i = 0; i < n && i < sizeof line_coding; i++) line_coding[i] = ep0_buf[i];
        pending_out = 0;
        ep0_status_in();
    } else {                                       /* status OUT de um IN: fim */
        ep0_in = 0;
        ep_stat_rx(0, RX_VALID);
    }
}

static void ep0_tx(void)
{
    ep_clear_ctr_tx(0);
    if (pending_addr) {                            /* status do SET_ADDRESS foi */
        USB_DADDR = USB_DADDR_EF | pending_addr;
        pending_addr = 0;
    }
    if (ep0_in) {
        if (ep0_left)     ep0_in_packet();
        else if (ep0_zlp) { ep0_zlp = 0; ep0_in_packet(); }   /* pacote vazio */
        else              ep0_in = 0;
    }
}

/* ---------------------------- EP1 (dados) ----------------------------- */
static void tx_packet(void)
{
    uint32_t n = tx_left > DATA_SIZE ? DATA_SIZE : tx_left;
    pma_write(PMA_EP1_TX, tx_ptr, n);
    BT_COUNT_TX(1) = (uint16_t)n;
    tx_ptr += n;
    tx_left -= n;
    tx_zlp = (tx_left == 0u && n == DATA_SIZE);   /* fecha a transferencia */
    ep_stat_tx(1, TX_VALID);
}

static void ep1_tx(void)
{
    ep_clear_ctr_tx(1);
    if (!tx_active) return;
    if (tx_left) {
        tx_packet();
    } else if (tx_zlp) {
        tx_zlp = 0;
        BT_COUNT_TX(1) = 0;
        ep_stat_tx(1, TX_VALID);
    } else {
        tx_active = 0;
    }
}

static void ep1_rx(void)
{
    uint8_t buf[DATA_SIZE];
    uint32_t n = BT_COUNT_RX(1) & 0x3FFu;
    if (n > DATA_SIZE) n = DATA_SIZE;
    pma_read(PMA_EP1_RX, buf, n);
    ep_clear_ctr_rx(1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (rx_head + 1u) % RX_RING;
        if (next == rx_tail) break;                /* cheio: descarta o resto */
        rx_ring[rx_head] = buf[i];
        rx_head = next;
    }
    ep_stat_rx(1, RX_VALID);
}

/* ------------------------------ geral --------------------------------- */
static void bus_reset(void)
{
    USB_BTABLE = 0;
    BT_ADDR_TX(0) = PMA_EP0_TX;  BT_COUNT_TX(0) = 0;
    BT_ADDR_RX(0) = PMA_EP0_RX;  BT_COUNT_RX(0) = COUNT_RX_64;
    BT_ADDR_TX(1) = PMA_EP1_TX;  BT_COUNT_TX(1) = 0;
    BT_ADDR_RX(1) = PMA_EP1_RX;  BT_COUNT_RX(1) = COUNT_RX_64;
    BT_ADDR_TX(2) = PMA_EP2_TX;  BT_COUNT_TX(2) = 0;
    BT_ADDR_RX(2) = 0;           BT_COUNT_RX(2) = 0;

    ep_setup(0, EP_CONTROL, RX_VALID, TX_NAK);
    USB_DADDR = USB_DADDR_EF;                      /* endereco 0 */

    ep0_in = 0;
    pending_addr = 0;
    pending_out = 0;
    set_configuration(0);
}

void usb_cdc_init(void)
{
    /* Desconecta: solta o pull-up interno e segura D+ (PA12) em 0 por um
     * tempo, para o host perceber a troca de dispositivo. */
    EXTEN_CTR &= ~EXTEN_USBD_PU_EN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    gpio_clr(GPIOA, 12);
    gpio_cfg(GPIOA, 12, GPIO_CRx_OUT_PP_2M);
    delay_ms(20);
    gpio_cfg(GPIOA, 12, GPIO_CRx_IN_FLOAT);

    RCC->APB1ENR  |=  RCC_APB1ENR_USBEN;
    RCC->APB1RSTR |=  RCC_APB1ENR_USBEN;
    RCC->APB1RSTR &= ~RCC_APB1ENR_USBEN;

    USB_CNTR = USB_CNTR_FRES;                      /* sai do power-down, ainda em reset */
    delay_ms(2);                                   /* tSTARTUP */
    USB_CNTR = 0;
    USB_ISTR = 0;
    bus_reset();

    EXTEN_CTR |= EXTEN_USBD_PU_EN;                 /* pull-up em D+: host enumera */
}

void usb_cdc_poll(void)
{
    uint32_t istr = USB_ISTR;

    if (istr & USB_ISTR_RESET) {
        USB_ISTR = (uint16_t)~USB_ISTR_RESET;
        bus_reset();
        return;
    }
    istr &= USB_ISTR_SUSP | USB_ISTR_WKUP | USB_ISTR_ERR | USB_ISTR_PMAOVR
          | USB_ISTR_SOF | USB_ISTR_ESOF;
    if (istr) USB_ISTR = (uint16_t)~istr;          /* so limpa (escrever 0 limpa) */

    while ((istr = USB_ISTR) & USB_ISTR_CTR) {
        uint32_t ep = istr & USB_ISTR_EP_ID;
        uint16_t r = (uint16_t)USB_EPR(ep);
        if (ep == 0u) {
            if (r & EP_CTR_RX) ep0_rx();
            if (r & EP_CTR_TX) ep0_tx();
        } else if (ep == 1u) {
            if (r & EP_CTR_RX) ep1_rx();
            if (r & EP_CTR_TX) ep1_tx();
        } else {
            if (r & EP_CTR_RX) ep_clear_ctr_rx(ep);
            if (r & EP_CTR_TX) ep_clear_ctr_tx(ep);
        }
    }
}

int usb_cdc_configured(void)
{
    return configured;
}

int usb_cdc_ready(void)
{
    return configured && dtr;
}

void usb_cdc_write(const uint8_t *data, uint32_t len)
{
    if (!configured || tx_active || len == 0u) return;
    tx_ptr = data;
    tx_left = len;
    tx_active = 1;
    tx_packet();
}

int usb_cdc_tx_busy(void)
{
    return tx_active;
}

int usb_cdc_getc(void)
{
    if (rx_tail == rx_head) return -1;
    uint8_t c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1u) % RX_RING;
    return c;
}
