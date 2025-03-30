/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/
#ifndef TXGEN_H
#define TXGEN_H

#include "common.h"

// Signal handling
void signal_handler_txgen(int signum);

// Transaction generation
Transaction generate_transaction(int reward);

#endif /* TXGEN_H */
