// USART1 on the TX/RX test points: 115200 8N1, interrupt RX, polled TX.
#include "board.h"
#include "kvm.h"

#define BAUD     115200u
#define RX_SIZE  64u     // power of two

static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint8_t rx_head, rx_tail;

void USART1_IRQHandler(void)
{
    uint32_t isr = USART1->ISR;
    if (isr & USART_ISR_ORE)
        USART1->ICR = USART_ICR_ORECF;
    while (USART1->ISR & USART_ISR_RXNE_RXFNE) {
        uint8_t c = (uint8_t)USART1->RDR;
        uint8_t next = (rx_head + 1) & (RX_SIZE - 1);
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

void uart_init(void)
{
    RCC->APBENR2 |= RCC_APBENR2_USART1EN;
    pa_pull(PIN_UART_RX, PULL_UP);      // idle high with nothing attached
    pa_af(PIN_UART_TX, 4);
    pa_af(PIN_UART_RX, 4);

    USART1->BRR = (SYSCLK_HZ + BAUD / 2) / BAUD;
    // RX FIFO covers the short interrupt-off window while LEDs update.
    USART1->CR1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_RE |
                  USART_CR1_RXNEIE_RXFNEIE | USART_CR1_UE;
    NVIC_EnableIRQ(USART1_IRQn);
}

int uart_getc(void)
{
    if (rx_tail == rx_head)
        return -1;
    uint8_t c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) & (RX_SIZE - 1);
    return c;
}

void uart_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF))
        ;
    USART1->TDR = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

void uart_putu(uint32_t v)
{
    char buf[11];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        uart_putc(buf[--i]);
}

void uart_puthex(uint32_t v, uint8_t digits)
{
    while (digits--)
        uart_putc("0123456789ABCDEF"[(v >> (digits * 4)) & 0xF]);
}
