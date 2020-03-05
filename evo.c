#include <avr/interrupt.h>
#include <avr/wdt.h>

#include "config.h"
#include "led.h"

#include "spi.h"
#include "cc1101.h"

#include "frame.h"
#include "message.h"
#include "tty.h"

void main_init(void) {
  // OSCCAL=((uint32_t)OSCCAL * 10368) / 10000;

#if defined(DEBUG_PORT)
  DEBUG_DDR  = DEBUG_MASK;
  DEBUG_PORT = 0;
#endif

  wdt_disable();
  led_init();
  tty_init();

  // Wire up components
  spi_init();
  cc_init();
  frame_init();
  msg_init( 18,0x48DADA );
  
  sei();
}

void main_work(void) {
  frame_work();
  msg_work();
  tty_work();
}

#ifdef NEEDS_MAIN
int main(void) {
  main_init();

  while(1) {
    main_work();
  }
}
#endif
