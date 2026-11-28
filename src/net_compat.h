#ifndef ARCANUM_NET_COMPAT_H_
#define ARCANUM_NET_COMPAT_H_

/*
 * Historical name: originally a stub layer so the game compiled without tig_net.
 * Coop Lite replaces the stubs with a real ENet-backed implementation in
 * src/net/tig_net.c. Single-player remains unchanged until -coop is passed.
 */
#include "net/tig_net.h"

#endif /* ARCANUM_NET_COMPAT_H_ */
