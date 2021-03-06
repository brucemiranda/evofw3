/***************************************************************
** sw_uart.c
**
** emulate a UART
**  RX: capturing edges and analyse to acquire byte
**  TX: generate edges
*/

#include "config.h"

#include <string.h>
#include <util/delay.h>

#include <avr/interrupt.h>

#include "frame.h"
#include "uart.h"

#define DEBUG_ISR(_v)      DEBUG1(_v)
#define DEBUG_EDGE(_v)     DEBUG2(_v)

/***************************************************************
** BIT constants
** These are based on a 500 KHz clock
** 500000/38400 is almost exactly 13
**
** Clock rates that are multiples of 500 KHz cause these
** constants to increase such that they do not all fit
** in uint8_t variables.
**
** Maintaining the variables as uint8_t significantly improves
** the efficiency of the RX processing code.
*/
#define ONE_BIT  13
#define HALF_BIT 7
#define BIT_TOL  4

#define MIN_BIT  ( ONE_BIT - BIT_TOL )
#define MAX_BIT  ( ONE_BIT + BIT_TOL )

#define NINE_BITS     ( 9 * ONE_BIT )
#define NINE_BITS_MIN ( NINE_BITS - HALF_BIT )
#define NINE_BITS_MAX ( NINE_BITS + HALF_BIT )

#define TEN_BITS      ( 10 * ONE_BIT )
#define TEN_BITS_MIN  ( TEN_BITS - HALF_BIT )
#define TEN_BITS_MAX  ( TEN_BITS + HALF_BIT )
#define STOP_BITS_MAX ( TEN_BITS + NINE_BITS - HALF_BIT  )

/***********************************************************************************
** RX Frame state machine
*/

enum uart_rx_states {
  RX_OFF,
  RX_IDLE,    // Make sure we've seen an edge for valid interval calculations
  RX_HIGH,    // Check HIGH signal, includes SYNC0 check (0xFF)
  RX_LOW,     // Check LOW signal
  RX_SYNC1,   // Check for SYNC1 (0x00) - revert to RX_HIGH if not found
  RX_STOP,    // Wait for STOP bit to obtain BYTE SYNCH
  RX_SYNCH,   // Gather bytes and report them to frame layer
  RX_SYNCH0,  // Clock recovery
};


#define MAX_EDGE 24
static struct uart_rx_state {
  uint16_t time;
  uint16_t lastTime;
  uint16_t time0;
  uint8_t  overflow;

  uint8_t level;
  uint8_t lastLevel;

  uint8_t state;
  uint8_t preamble;

  uint8_t nByte;
  uint8_t lastByte;

  // Edge buffers
  uint8_t Edges[2][MAX_EDGE];
  uint8_t NEdges[2];

  // Current edges
  uint8_t idx;
  uint8_t nEdges;
  uint8_t *edges;
} rx;

static void rx_reset(void) {
  memset( &rx, 0, sizeof(rx) );
  rx.edges = rx.Edges[ rx.idx ];
}

/***********************************************************************************
** Byte synchronisation
**
** If we see something that looks like SYNC0 (0xFF) we'll explitly
** check for SYNC1 (0x00). If we see that we'll decide we've got the sync word
**
** Once we've made this decision we'll just wait for the STOP BIT so
** we can get BYTE synchronisation.
**
** This is a very lightweight approach to detecting start of frame
** when there's likely to be a lot of noise
*/

//-----------------------------------------------------------------------------
// RX reset assumes last edge was falling edge
// Make sure we've seen a rising edge before we do interval measurement
static uint8_t rx_idle( void) {
  uint8_t state = RX_IDLE;

  if( rx.level )
    state = RX_HIGH;

  return state;
}

//-----------------------------------------------------------------------------
// check high signals
static uint8_t rx_high( uint8_t interval ) {
  uint8_t state = RX_HIGH;    // Stay here until we see a LOW

  if( !rx.level ) { // falling edge
   if( interval >= NINE_BITS_MIN )
      state = RX_SYNC1;  // This was SYNC0, go look explicitly for SYNC1
    else
      state = RX_LOW;
  }

  return state;
}

//-----------------------------------------------------------------------------
// check low signals
static uint8_t rx_low( uint8_t interval __attribute__((unused))) {
  uint8_t state = RX_LOW;    // Stay here until we see a HIGH

  if( rx.level ) { // rising edge
    state = RX_HIGH;
  }

  return state;
}

static uint8_t rx_sync1( uint8_t interval ) {
  uint8_t state = RX_SYNC1;    // Stay here until we see a HIGH

  if( rx.level ) {  // rising edge

    // NOTE: we're accepting 9 or 10 bits here because of observed behaviour
    if( interval >= NINE_BITS_MIN && interval <= TEN_BITS_MAX )
      state = RX_STOP;  // Now we just need the STOP bit for BYTE synch
    else
      state = RX_HIGH;
  }

  return state;
}

//-----------------------------------------------------------------------------
// wait for end of STOP BIT
static uint8_t rx_stop_bit( uint8_t interval __attribute__((unused))) {
  uint8_t state = RX_STOP;  // Stay here until we see a LOW

  if( !rx.level ) { // falling edge
    // NOTE: we're not going to validate the STOP bit length
    // Observed behavior of some devices is to generate extended ones
    // If we have mistaken the SYNC WORD we'll soon fail.
    state = RX_SYNCH0;
  }

  return state;
}

//-----------------------------------------------------------------------------
// gather bytes for frame

static void rx_byte(void) {
  rx.nByte++;

  // Switch edge buffer
  rx.NEdges[rx.idx] = rx.nEdges;
  rx.idx ^= 1;
  rx.edges = rx.Edges[rx.idx];
  rx.nEdges = 0;

  SW_INT_PIN |= SW_INT_IN;
}

static uint8_t rx_abort(uint8_t code) {
  rx.lastByte = code;
  rx_byte();
  return ( rx.level ) ? RX_HIGH : RX_SYNC1;
}

static uint8_t rx_synch(uint8_t interval) {
  uint8_t state = RX_SYNCH;

  if( rx.nEdges < MAX_EDGE ) {
    rx.edges[rx.nEdges++] = interval;

    if( interval>TEN_BITS_MIN ) {
      if( interval < STOP_BITS_MAX ) { // Possible stop bit
        if( !rx.level ) { // Was a falling edge so probably valid stop bit
          rx_byte();
          state = RX_SYNCH0;
        } else { // Lost BYTE synch
          state = rx_abort(FRM_LOST_SYNC);
        }
      } else { // lost BYTE synch
        state = rx_abort(FRM_LOST_SYNC);
      }
    } else if( rx.lastByte==0x35 ) {
      state = RX_IDLE;
    }
  } else { // Too many edges
    state = rx_abort(FRM_LOST_SYNC);
  }

  return state;
}

/***************************************************************************
** RX edge processing
*/
static uint8_t rx_edge(uint8_t interval) {
  uint8_t synch = 1;

  switch( rx.state ) {
  case RX_IDLE:   rx.state = rx_idle();               break;

  // Frame detect states
  case RX_LOW:    rx.state = rx_low(interval);        break;
  case RX_HIGH:   rx.state = rx_high(interval);       break;
  case RX_SYNC1:  rx.state = rx_sync1(interval);      break;
  case RX_STOP:   rx.state = rx_stop_bit(interval);   break;

  // Byte gathering
  case RX_SYNCH0: // SYNCH00 Only used to signal clock recovery
  case RX_SYNCH:  rx.state = rx_synch(interval);      break;

  default:
    break;
  }

  // When we're gathering bytes only synch time0 at the end of bytes
  // This allows us to perform clock recovery on the stop/start bit
  // boundary.
  if( rx.state == RX_SYNCH )
    synch = 0;

  return synch;
}

/***********************************************************************************
** RX
** On Edge interrupts from the radio signal use the counter as a timer
**
** The difference between the counts on 2 successive edges gives the width
** of a SPACE or MARK period in the signal immediately before the latest edge
**
*/

#define RX_CLOCK TCNT1

static uint8_t clockShift;

static void rx_edge_detected(void) {
  uint16_t interval;
  uint8_t synch;
  
  if( rx.overflow && ( ( rx.overflow > 1 ) || ( rx.time > rx.time0 ) ) ) {
      interval = 255;
  } else {
    interval = ( rx.time - rx.time0 ) >> clockShift;
    if( interval > 255 ) interval = 255;
  }
  rx.overflow = 0;
  
  synch = rx_edge( interval );
  if( synch ) rx.time0 = rx.time;
  rx.lastLevel = rx.level;
  rx.lastTime  = rx.time;
}

ISR(GDO2_INT_VECT) {
  DEBUG_ISR(1);

  rx.time  = RX_CLOCK;                // Grab a copy of the counter ASAP for accuracy
  rx.level = ( GDO2_PIN & GDO2_IN );  // and the current level

  if( rx.level != rx.lastLevel )
	rx_edge_detected();

  DEBUG_ISR(0);
}

ISR(TIMER1_OVF_vect) {
  rx.overflow += 1;
  if( rx.overflow > 1 )
    rx_edge_detected();
}

/***************************************************************************
** Enable a free-running counter that gives us a time reference for RX
*/

static void rx_init(void) {
  TCCR1A = 0; // Normal mode, no output pins

  // We want to prescale the hardware timer as much as possible
  // to maximise the period between overruns but remain above 500 KHz
  TCCR1B = ( 1<<CS11 ); // Pre-scale by 8

  // This is the additional scaling required in software to reduce the
  // clock rate to 500 KHz
  clockShift = ( F_CPU==16000000 ) ? 2 : 1;

  TIMSK1 |= TOIE1;
}

/********************************************************
** Edge analysis ISR
** In order to avoid delaying the measurement of edges
** the analysis of the edges is run in a lowewr priority
** ISR.
********************************************************/

static uint8_t rx_process_edges( uint8_t *edges, uint8_t nEdges ) {
  uint8_t rx_byte = 0;
  uint8_t rx_t = 0;
  uint8_t rx_tBit = ONE_BIT;
  uint8_t rx_isHi = 0;
  uint8_t rx_hi = 0;

  while( nEdges-- ) {
    uint8_t interval = *(edges++);

    if( rx_tBit < TEN_BITS ) { //
      uint8_t samples = interval - rx_t;
      while( samples ) {
        uint8_t tBit = rx_tBit - rx_t;
        if( tBit > samples )
          tBit = samples;

        if( rx_isHi ) rx_hi += tBit;

        rx_t += tBit;
        samples -= tBit;

        // BIT complete?
        if( rx_t==rx_tBit ) {
          if( rx_tBit == ONE_BIT ) { // START BIT
          } else if( rx_tBit < TEN_BITS ) {
            uint8_t bit = ( rx_hi > HALF_BIT ) ? 0x80 : 0x00 ;
            rx_byte >>= 1;
            rx_byte  |= bit;
          }

          rx_tBit += ONE_BIT;
          rx_hi = 0;
        }
      }
    }

    // Edges toggle level
    rx_isHi ^= 1;
  }

  return rx_byte;
}

ISR(SW_INT_VECT) {
  // Very important that we don't block interrupts
  // As this interferes with subsequent edge measurements
  sei();

  DEBUG_EDGE( 1 );

  // Extract byte from previous edges
  rx.lastByte = rx_process_edges( rx.Edges[1-rx.idx], rx.NEdges[1-rx.idx] );

  DEBUG_EDGE( 0 );

  // And pass it on to frame to process
  frame_rx_byte( rx.lastByte );

}

//---------------------------------------------------------------------------------

static void rx_start(void) {
  uint8_t sreg = SREG;
  cli();

  // rising and falling edge
  EICRA &= ~( 1 << GDO2_INT_ISCn0 ) & ~( 1 << GDO2_INT_ISCn1 ) ;
  EICRA |=  ( 1 << GDO2_INT_ISCn0 );   

  EIFR   = GDO2_INT_MASK ;    // Acknowledge any previous edges
  EIMSK |= GDO2_INT_MASK ;    // Enable interrupts

  // Configure SW interrupt for edge processing
  SW_INT_DDR  |= SW_INT_IN;
  SW_INT_MASK |= SW_INT_IN;

  PCIFR  = SW_INT_ENBL;  // Acknowledge any previous event
  PCICR |= SW_INT_ENBL;  // and enable

  SREG = sreg;
}

//---------------------------------------------------------------------------------

static void rx_stop(void) {
  EIMSK &= ~GDO2_INT_MASK;  // Disable interrupts
  rx.state = RX_OFF;
}


/***************************************************************************
** TX Processing
*/

enum uart_tx_states {
  TX_OFF,
  TX_IDLE,
  TX
};


#define MARK  1
#define SPACE 0

static struct uart_tx_state {
  uint8_t state;

  uint8_t byte;
  uint8_t bitNo;
} tx;

static void tx_reset(void) {
  memset( &tx, 0, sizeof(tx) );
}

ISR(TIMER0_COMPA_vect) {
  uint8_t bit;
  DEBUG_ISR(1);

  if( tx.bitNo==0 ) { // START bit
	bit = SPACE;
  } else if( tx.bitNo<9 ) { // data bit
    uint8_t mask = 1 << ( tx.bitNo - 1 );	// Little Endian
    bit = ( tx.byte & mask ) ? MARK : SPACE;
  } else { // STOP bit
    bit = MARK;
  }

  if( bit ) GDO0_PORT |=  GDO0_IN ;
  else      GDO0_PORT &= ~GDO0_IN ;

  if( tx.bitNo==0 ) tx.byte = frame_tx_byte();
  tx.bitNo = ( tx.bitNo+1 ) % 10;

  DEBUG_ISR(0);
}

//---------------------------------------------------------------------------------

static void tx_init(void) {
  TCCR0A = ( 1<<WGM01 ); // CTC, no output pins
  TCCR0B = 0 ;
	
  TCCR0B |= ( 1<<CS01 ); // Pre-scale by 8

  OCR0A = 51;	// 38400 baud	
}

//---------------------------------------------------------------------------------

static void tx_start(void) {
  uint8_t sreg = SREG;
  cli();

  TCNT0 = 0;
  TIMSK0 |= ( 1<<OCIE0A );
  GDO0_PORT |=  GDO0_IN ;	// Start in MARK

  SREG = sreg;
}

//---------------------------------------------------------------------------------

static void tx_stop(void) {
  TIMSK0 &= ~( 1<<OCIE0A );
  tx.state = TX_OFF;
  GDO0_PORT &= ~GDO0_IN ;	// Leave in SPACE
}


/***************************************************************************
** External interface
*/

void uart_rx_enable(void) {
  uint8_t sreg = SREG;
  cli();

  tx_stop();
  rx_reset();
  rx.state = RX_IDLE;

  SREG = sreg;

  rx_start();
}

void uart_tx_enable(void) {
  uint8_t sreg = SREG;
  cli();

  rx_stop();
  tx_reset();
  tx.state = TX_IDLE;

  SREG = sreg;

  tx_start();
}

void uart_disable(void) {
  uint8_t sreg = SREG;
  cli();
	
  rx_stop();
  tx_stop();

  SREG = sreg;
}

void uart_init(void) {
  uint8_t sreg = SREG;
  cli();

  GDO0_DDR  |=  GDO0_IN;
  GDO0_PORT &= ~GDO0_IN;		// Sstart in SPACE

  GDO2_DDR  &= ~GDO2_IN;
  GDO2_PORT |=  GDO2_IN;		// Set input pull-up

  rx_init();
  tx_init();  

  SREG = sreg;
}

