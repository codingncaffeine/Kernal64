/* Pins of the transistor-level 6502 (perfect6502) that the differential test
 * drives, by their node numbers in its netlist. All are active low on the chip. */
#include "types.h"
#include "netlist_sim.h"

enum { NODE_IRQ = 103, NODE_NMI = 1297, NODE_RDY = 89, NODE_SO = 1672 };

void p6502_set_irq(void* s, int asserted) { setNode(s, NODE_IRQ, !asserted); }
void p6502_set_nmi(void* s, int asserted) { setNode(s, NODE_NMI, !asserted); }
void p6502_set_rdy(void* s, int high) { setNode(s, NODE_RDY, high); }
void p6502_set_so(void* s, int asserted) { setNode(s, NODE_SO, !asserted); }
