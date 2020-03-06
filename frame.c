#include <string.h>
#include <util/delay.h>

#include <avr/interrupt.h>

#include "config.h"
#include "cc1101.h"
#include "message.h"
#include "frame.h"

#define DEBUG_ISR(_v)      DEBUG1(_v)
#define DEBUG_EDGE(_v)     DEBUG2(_v)
#define DEBUG_FRAME(_v)    DEBUG3(_v)

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
#define BAUD_RATE 38400

#define ONE_BIT  13
#define HALF_BIT 7
#define BIT_TOL  4

#define MIN_BIT  ( ONE_BIT - BIT_TOL )
#define MAX_BIT  ( ONE_BIT + BIT_TOL )

#define NINE_BITS		( 9 * ONE_BIT )
#define NINE_BITS_MIN	( NINE_BITS - HALF_BIT )
#define NINE_BITS_MAX	( NINE_BITS + HALF_BIT )

#define TEN_BITS		( 10 * ONE_BIT )
#define TEN_BITS_MIN	( TEN_BITS - HALF_BIT )
#define TEN_BITS_MAX	( TEN_BITS + HALF_BIT )
#define STOP_BITS_MAX   ( 14 * ONE_BIT + HALF_BIT )

/***********************************************************************************
** RX Frame state machine
*/

enum rx_states {
  RX_OFF,
  RX_IDLE,		// Make sure we've seen an edge for valid interval calculations
  // FRAME DETECT states, keep track of preamble/training bits
  RX_HIGH, 		// Check HIGH signal, includes SYNC0 check (0xFF)
  RX_LOW, 		// Check LOW signal
  RX_SYNC1,		// Check for SYNC1 (0x00) - revert to RX_HIGH if not found
  RX_STOP,		// Wait for STOP bit to obtain BYTE SYNCH
  // FRAME PROCESS  states
  RX_FRAME0,	// First edge in byte within frame
  RX_FRAME,		// Rest of byte
  RX_DONE		// End of frame reached - discard everything
};

static struct rx_state {
  uint16_t time;
  uint16_t lastTime;
  uint16_t time0;

  uint8_t level;
  uint8_t lastLevel;

  uint8_t state;
  uint8_t preamble;
  
  uint8_t nByte;
  uint8_t lastByte;

  // Edge buffers
  uint8_t Edges[2][24];
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

static void rx_frame_start(void);
static void rx_frame_end(void);
static void rx_frame_done(void);

static void rx_stop(void);
static void rx_start(void);

/***********************************************************************************
** FRAME detection
**
** A frame should begin with ...0101s<FF>ps<00>p...
**   ...<training >< SYNC WORD>...
**
** However, we have seen devices break this pattern
** e.g. no preamble bits from HR80s
**      extended STOP bit from DTS92 and possibly others
**      extended SYNC1 from DTS92(?)
**
** To detect frames we'll just monitor intervals of HIGH and LOW signal.
**
** If we see something that looks like SYNC0 (0xFF) we'll explitly
** check for SYNC0 (0x00). If we see that we'll decide we've seen a frame.
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
// Keep track of preamble/training bits
static void rx_preamble( uint8_t interval ) {
  if( interval >= MIN_BIT && interval <= MAX_BIT ) {
    // make sure we don't overflow
	if( rx.preamble < 8*8 )
      rx.preamble++;
  } else {
    rx.preamble = 0;
  }
}

//-----------------------------------------------------------------------------
// check high signals
static uint8_t rx_high( uint8_t interval ) {
  uint8_t state = RX_HIGH;		// Stay here until we see a LOW

  if( !rx.level ) { // falling edge
   if( interval >= NINE_BITS_MIN ) //&& interval <= NINE_BITS_MAX )
      state = RX_SYNC1;	// This was SYNC0, go look explicitly for SYNC1
    else
      state = RX_LOW;

    rx_preamble( interval );
  }
  
  return state;
}

//-----------------------------------------------------------------------------
// check low signals
static uint8_t rx_low( uint8_t interval ) {
  uint8_t state = RX_LOW;		// Stay here until we see a HIGH

  if( rx.level ) { // rising edge
    state = RX_HIGH;

    rx_preamble( interval );
  }
  
  return state;
}

static uint8_t rx_sync1( uint8_t interval ) {
  uint8_t state = RX_SYNC1;		// Stay here until we see a HIGH

  if( rx.level ) {  // rising edge
  
    // NOTE: we're accepting 9 or 10 bits here because of observed behaviour
    if( interval >= NINE_BITS_MIN && interval <= TEN_BITS_MAX )
	  state = RX_STOP;	// Now we just need the STOP bit for BYTE synch
    else
	  state = RX_HIGH;

    rx_preamble( interval );
  }

  return state;
}

//-----------------------------------------------------------------------------
// wait for end of STOP BIT
static uint8_t rx_stop_bit( uint8_t interval __attribute__ ((unused)) ) {
  uint8_t state = RX_STOP;		// Stay here until we see a LOW

  if( !rx.level ) {  // falling edge
    // NOTE: we're not going to validate the STOP bit length
	// Observed behavior of some devices is to generate extended ones
	// If we have mistaken the SYNC WORD we'll soon fail.
	  state = RX_FRAME0;
	  rx_frame_start();
  }
  
  return state;
}



/***************************************************************************
** RX frame processing
*/

static void rx_frame_start(void) {
  DEBUG_FRAME(1);
  msg_rx_byte(MSG_START);
}

static void rx_frame_end(void) {
  DEBUG_FRAME(0);
  rx_stop();
}

static void rx_frame_done(void) {
  uint8_t rssi = cc_read_rssi();
  msg_rx_rssi( rssi );
  msg_rx_byte(MSG_END);
};

static void rx_byte(void) {
  rx.nByte++;
  
  // Switch edge buffer
  rx.NEdges[rx.idx] = rx.nEdges;
  rx.idx ^= 1;
  rx.edges = rx.Edges[rx.idx];
  rx.nEdges = 0;

  SW_INT_PIN |= SW_INT_IN;
}

static uint8_t rx_frame(uint8_t interval) {
  uint8_t state = RX_FRAME;

  rx.edges[rx.nEdges++] = interval;
  
  if( interval>TEN_BITS_MIN ) {
    if( interval < STOP_BITS_MAX ) { // Possible stop bit
	  if( !rx.level ) { // Was a falling edge so probably valid stop bit
	    rx_byte();
		state = RX_FRAME0;
	  } else { // Lost BYTE synch 
        rx_frame_end();
        state = RX_DONE;
	  }
    } else { // lost BYTE synch
      rx_frame_end();
      state = RX_DONE;
    }
  } else if( rx.lastByte==0xAC ) {
    rx_frame_end();
    state = RX_DONE;
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

  // Frame processing
  case RX_FRAME0: // FRAME0 Only used to signal clock recovery
  case RX_FRAME:  rx.state = rx_frame(interval);      break;
  }

  // When we're in a frame mode only synch time0 at the end of bytes
  // This allows us to perform clock recovery on the stop/start bit
  // boundary.
  if( rx.state == RX_FRAME )
    synch = 0;
  
  return synch;
}


/***********************************************************************************
** RX
** On Edge interrupts from the radio signal use the counter as a timer
**
** The difference between the counts on 2 successive edges gives the width
** of a LOW or HIGH period in the signal immediately before the latest edge
**
*/

#define RX_CLOCK TCNT1

static uint8_t clockShift;

ISR(GDO2_INT_VECT) {
  DEBUG_ISR(1);

  rx.time  = RX_CLOCK;                // Grab a copy of the counter ASAP for accuracy
  rx.level = ( GDO2_PIN & GDO2_IN );  // and the current level

  if( rx.level != rx.lastLevel ) {
    uint8_t interval = ( rx.time - rx.time0 ) >> clockShift;

	uint8_t synch = rx_edge( interval );
	if( synch ) rx.time0 = rx.time;

    rx.lastLevel = rx.level;
    rx.lastTime  = rx.time;
  }

  DEBUG_ISR(0);
}


/***************************************************************************
** Enable a free-running counter that gives us a time reference for RX
*/

static void rx_init(void) {
  uint8_t sreg = SREG;
  cli();

  TCCR1A = 0; // Normal mode, no output pins

  // We want to prescale the hardware timer as much as possible
  // to maximise the period between overruns but remain above 500 KHz
  TCCR1B = ( 1<<CS11 ); // Pre-scale by 8

  // This is the additional scaling required in software to reduce the 
  // clock rate to 500 KHz
  clockShift = ( F_CPU==16000000 ) ? 2 : 1;

  SREG = sreg;
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

  DEBUG_EDGE( 1 );

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
		  }
          else if( rx_tBit < TEN_BITS ) {  
            uint8_t bit = ( rx_hi > HALF_BIT );
            rx_byte <<= 1;
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

  DEBUG_EDGE( 0 );

  return rx_byte;
}

ISR(SW_INT_VECT) {
  // Very important that we don't block interrupts
  // As this interferes with subsequent edge measurements
  sei();

  // Extract byte from previous edges
  rx.lastByte = rx_process_edges( rx.Edges[1-rx.idx], rx.NEdges[1-rx.idx] );
  
  // And pass it on to message to process
  msg_rx_byte( rx.lastByte );
}


//---------------------------------------------------------------------------------

static void rx_start(void) {
  uint8_t sreg = SREG;
  cli();

  // Make sure configured as input in case shared with TX
  GDO2_DDR  &= ~GDO2_IN;
  GDO2_PORT |=  GDO2_IN;		 // Set input pull-up
  
  EICRA |= ( 1 << GDO2_INT_ISCn0 );   // rising and falling edge
  EIFR   = GDO2_INT_MASK ;     // Acknowledge any previous edges
  EIMSK |= GDO2_INT_MASK ;     // Enable interrupts
  
  // Configure SW interrupt for edge processing
  SW_INT_DDR  |= SW_INT_IN;
  SW_INT_MASK |= SW_INT_IN;

  PCIFR  = SW_INT_ENBL;	// Acknowledge any previous event
  PCICR |= SW_INT_ENBL;	// and enable
  
  SREG = sreg;
}

//---------------------------------------------------------------------------------

static void rx_stop(void) {
  uint8_t sreg = SREG;
  cli();

  EIMSK &= ~GDO2_INT_MASK;                 // Disable interrupts
  
  SREG = sreg;
  
  // TODO: put the radio in IDLE mode
}

/***********************************************************************************
** TX Processing
*/
enum tx_states {
  TX_OFF,
  TX_IDLE,
  TX_PREAMBLE,
  TX_SYNC,
  TX_MSG,
  TX_TRAIN,
  TX_DONE
};

static uint8_t tx_idle(void);
static uint8_t tx_preamble(void);
static uint8_t tx_sync(void);
static uint8_t tx_msg(void);
static uint8_t tx_train(void);
static uint8_t tx_done(void);

static struct tx_state {
  uint8_t state;
  uint8_t count;
  
  uint8_t byte;
  uint8_t bit;
  
  struct message *msg;
} tx;

static void tx_reset(void) {
  memset( &tx, 0, sizeof(tx) );
}

static void tx_start(void);
static void tx_stop(void);

static void tx_frame_start(void);
static void tx_frame_end(void);
static void tx_frame_done(void);

static void tx_set_byte(uint8_t byte);

/***********************************************************************************
** FRAME transmission
**
** A TX frame consists of 
**   <PREAMBLE><SYNC WORD><MSG><TRAINING>
**
** In each part of the frame apart from MSG the bytes to be transmitted
** are pre-defined.
**
*/

#define TRAIN 0xAA
#define TX_PREAMBLE_LEN 4
#define TX_TRAIN_LEN    2

#define SYNC0 0xFF
#define SYNC1 0x00

static uint8_t tx_idle( void ) {
  tx.count = 0;
  
  if( !tx.msg )
	return tx_done();
  else {
	tx_frame_start();
    return tx_preamble();
  }
}

static uint8_t tx_preamble( void ) {
  uint8_t state = TX_PREAMBLE;
  
  if( tx.count <TX_PREAMBLE_LEN ) {
	tx_set_byte(TRAIN);
  } else {
	tx.count = 0;
	state = tx_sync();
  }
	
  return state;
}

static uint8_t tx_sync( void ) {
  static uint8_t const sync[2] = { SYNC0,SYNC1  };
  uint8_t state = TX_SYNC;
  
  if( tx.count<sizeof(sync) ) {
	tx_set_byte( sync[tx.count] );
  } else {
	tx.count = 0;
	state = tx_msg();
  }
	
  return state;
}

static uint8_t tx_msg( void ) {
  uint8_t state = TX_MSG;
  uint8_t byte = msg_tx_byte( tx.msg );
  
  if( byte ) {
	tx_set_byte( byte );
  } else {
	tx.count = 0;
	state = tx_train();
  }
	
  return state;
}

static uint8_t tx_train( void ) {
  uint8_t state = TX_TRAIN;
  
  if( tx.count <TX_TRAIN_LEN ) {
	tx_set_byte(TRAIN);
  } else {
	tx.count = 0;
    tx_frame_end();
	state = tx_done();
  }
	
  return state;
}

static uint8_t tx_done( void ) {
  return TX_DONE;
}

/***************************************************************************
** TX frame processing
*/

static void tx_frame_start(void) {
  DEBUG_FRAME(1);
}

static void tx_frame_end(void) {
  DEBUG_FRAME(0);
  tx_stop();
}

static void tx_frame_done(void) {
  msg_tx_done( &tx.msg );
};

#define TX_START_BIT 10
#define TX_STOP_BIT   1

static void tx_set_byte(uint8_t byte) {
  tx.byte = byte;
  tx.bit = TX_START_BIT;
  tx.count++;
}

static void tx_bit( uint8_t level ) {
  if( level ) GDO0_PORT |=  GDO0_IN;
  else        GDO0_PORT &= ~GDO0_IN;
}

static void tx_frame(void) {
  if( tx.state==TX_OFF ) return;

  if( tx.bit == TX_START_BIT ) {
	tx_bit( 0 );
  } else if( tx.bit == TX_STOP_BIT ) {
	tx_bit( 1 );
  } else {
	tx_bit( tx.byte & 0x80 );
	tx.byte <<= 1;
  }
  tx.bit--;

  if( tx.bit == 0 ) {	
    switch( tx.state ) {
    case TX_IDLE:     tx.state = tx_idle();		break;
    case TX_PREAMBLE: tx.state = tx_preamble();	break;
    case TX_SYNC:     tx.state = tx_sync();     break;
    case TX_MSG:      tx.state = tx_msg();      break;
    case TX_TRAIN:    tx.state = tx_train();    break;
    case TX_DONE:     tx.state = tx_done();     break;
	}
  }
}

/***********************************************************************************
** TX
** On clock interrupts send the next bit to the radio
*/

ISR(TIMER0_COMPA_vect) {
  DEBUG_ISR(1);
  tx_frame();
  DEBUG_ISR(0);
}

static void tx_start(void) {
  uint8_t sreg = SREG;
  cli();

  GDO0_PORT |=  GDO0_IN;		 // output high
  GDO0_DDR  |=  GDO0_IN;

  TIFR0  = 0;
  TIMSK0 = ( 1<<OCIE0A );

  SREG = sreg;
}

static void tx_stop(void) {
  uint8_t sreg = SREG;
  cli();

  TIMSK0 = ( 0<<OCIE0A );

  SREG = sreg;
}

// Timer at bitrate interval.
static void tx_init(void) {
 uint32_t temp;
  
  uint8_t sreg = SREG;
  cli();

  // Timer/Counter 0 : CTC, no output pins, F_CPU/8
  TCCR0A = 0x02;
  TCCR0B = 0x02;

  temp  = F_CPU / 8;  // Pre scaler
  temp /= BAUD_RATE;  // counts/BIT
  OCR0A = temp;

  SREG = sreg;
}

/***************************************************************************
** External interface
*/

void frame_rx_enable(void) {
  uint8_t sreg = SREG;
  cli();
  
  rx_reset();
  rx.state = RX_IDLE;

  SREG = sreg;
    
  rx_start();
  cc_enter_rx_mode();
}

void frame_rx_disable(void) {
  cc_enter_idle_mode();
  rx_stop();
}

void frame_tx_enable(void) {
  cc_enter_tx_mode();
  tx.state = TX_IDLE;
  tx.bit = 1;
  tx_start();
}

void frame_tx_disable(void) {
  tx_stop();
  cc_enter_idle_mode();

  tx_frame_done();
  tx_reset();
}

void frame_init(void) {
  rx_reset();
  rx_init();

  tx_reset();
  tx_init();
}

#define RX
#define TX

void frame_work(void) {

  if( rx.state==RX_DONE ) {
	rx_frame_done();
    frame_rx_enable();
  }
  if( rx.state==RX_OFF ) {
#ifdef RX
    frame_rx_enable();
#endif	
  }
  
  if( !tx.msg ) {
	tx.msg = msg_tx_get();
  } else {  // TX pending
	// TODO: verify safe to TX

#ifdef TX
    if( tx.state == TX_OFF  ) { 
	  frame_rx_disable(); 
	  frame_tx_enable(); 
	}
    if( tx.state == TX_DONE ) { 
	  frame_tx_disable(); 
	  rx_reset();
	}
#else
    tx_frame_done();
#endif
  }
  
  
}
