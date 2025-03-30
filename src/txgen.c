/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include "logger.h"
#include "txgen.h"


#define DEBUG 1

// ./txgen <reward> <sleep_time>
// Sleep time is in milliseconds
int running = 1;
int num_transactions = 0;

void signal_handler_txgen(int signum) {
    if (signum == SIGINT) {
        log_message("Received SIGINT from txgen, shutting down txgen...\n");
        running = 0;
    }
}


int main(int argc, char *argv[]){
    if (argc != 3) {
        fprintf(stderr, "Please use the following format: %s <reward> <sleep_time>\n", argv[0]);
        exit(1);
    }
    int reward = atoi(argv[1]);
    int sleep_time = atoi(argv[2]);

    if (reward < 1 || reward > 3 || sleep_time < 200 || sleep_time > 3000) {
        fprintf(stderr, "Invalid reward value, must be between 1 and 3\n");
        fprintf(stderr, "Invalid sleep time, must be between 200ms and 3000ms\n");
        exit(1);
    }

    // Logging new session 
    #ifdef DEBUG
    log_message("New TXGEN session started\n");
    #endif

    // Setup the signal handler for SIGINT
    signal(SIGINT, signal_handler_txgen);

    // Initialize the logger
    logger_init("DEIChain_log.txt");

    #ifdef DEBUG
    log_message("TxGen starting up\n");
    #endif

    // Create a random number generator
    srand(time(NULL));

    // Main loop
    while (running) {
        // Generate a random transaction    
        Transaction tx;
        tx.id = rand() % 100;
        tx.value = (float)(rand() % 1000) / 3.0;
        tx.timestamp = time(NULL);

        // Log the transaction
        #ifdef DEBUG
        log_message("Transaction generated: %d Reward: %d BTC Sleep time: %dms Value: %.2f\n", tx.id, reward, sleep_time, tx.value);
        #endif
        
        // Increment the number of transactions
        num_transactions++;

        // Sleep for the specified time 
        usleep(sleep_time * 1000);

        // Sleep for 5 seconds
        sleep(5);
    }

    // Close the logger
    logger_close();

    return 0;
}






