/* pow.c - Proof-of-Work implementation */

#include "pow.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "logger.h"

int get_max_transaction_reward(const Block *block, const int txs_per_block) {
    if (block == NULL) {
        #if DEBUG
        debug_message("POW: Block pointer is NULL in get_max_transaction_reward\n");
        #endif
        return 0;
    }

    if (txs_per_block <= 0) {
        #if DEBUG
        debug_message("POW: Invalid txs_per_block (%d) in get_max_transaction_reward\n", txs_per_block);
        #endif
        return 0;
    }

    #if DEBUG
    debug_message("POW: Checking rewards for %d transactions\n", txs_per_block);
    #endif
    int max_reward = 0;

    for (int i = 0; i < txs_per_block; ++i) {
        int reward = block->transactions[i].reward;
        #if DEBUG
        debug_message("POW: Transaction %d has reward %d\n", i, reward);
        #endif
        
        // Validate reward value
        if (reward < 1 || reward > 3) {
            #if DEBUG
            debug_message("POW: Invalid reward value %d in transaction %d\n", reward, i);
            #endif
            continue;
        }

        if (reward > max_reward) {
            max_reward = reward;
        }
    }

    #if DEBUG
    debug_message("POW: Maximum reward found: %d\n", max_reward);
    #endif
    return max_reward;
}

unsigned char *serialize_block(const Block *block, size_t *sz_buf, int transactions_per_block) {
    if (!block) {
        log_message("POW: NULL block pointer in serialize_block\n");
        return NULL;
    }
    
    //log_message("POW: Serializing block with %d transactions, nonce: %d\n", 
    //          transactions_per_block, block->nonce);
    
    // Calculate the size needed for the buffer
    *sz_buf = sizeof(int) +                    // txb_id
              HASH_SIZE +                      // prev_hash
              sizeof(time_t) +                 // txb_timestamp
              (transactions_per_block * sizeof(Transaction)) + // transactions
              sizeof(int);                     // nonce

    unsigned char *buffer = malloc(*sz_buf);
    if (!buffer) {
        log_message("POW: Failed to allocate serialization buffer\n");
        return NULL;
    }

    memset(buffer, 0, *sz_buf);

    unsigned char *p = buffer;

    // Copy txb_id
    memcpy(p, &block->txb_id, sizeof(int));
    p += sizeof(int);

    // Copy previous block hash
    memcpy(p, block->prev_hash, HASH_SIZE);
    p += HASH_SIZE;

    // Copy timestamp
    memcpy(p, &block->txb_timestamp, sizeof(time_t));
    p += sizeof(time_t);

    // Copy all transactions
    for (int i = 0; i < transactions_per_block; ++i) {
        memcpy(p, &block->transactions[i], sizeof(Transaction));
        p += sizeof(Transaction);
    }

    // Copy nonce
    memcpy(p, &block->nonce, sizeof(int));

    return buffer;
}

/* Function to compute SHA-256 hash */
void compute_sha256(const Block *block, char *output, int transactions_per_block) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    size_t buffer_sz;

    // since the Block has a pointer we must serialize the block to a buffer
    unsigned char *buffer = serialize_block(block, &buffer_sz, transactions_per_block);

    SHA256(buffer, buffer_sz, hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[SHA256_DIGEST_LENGTH * 2] = '\0';
    free(buffer);
}

/* Function to check difficulty using fractional levels */
int check_difficulty(const char *hash, const int reward) {
    int minimum = 4;  // minimum difficulty

    int zeros = 0;
    DifficultyLevel difficulty = getDifficultFromReward(reward);

    while (hash[zeros] == '0') {
        zeros++;
    }

    // At least `minimum` zeros must exist
    if (zeros < minimum) return 0;

    char next_char = hash[zeros];

    switch (difficulty) {
        case EASY:  // 0000[0-b]
            if ((zeros == 4 && next_char <= 'b') || zeros > 4) return 1;
            break;
        case NORMAL:  // 00000
            if (zeros >= 5) return 1;
            break;
        case HARD:  // 00000[0-b]
            if ((zeros == 5 && next_char <= 'b') || zeros > 5) return 1;
            break;
        default:
            #if DEBUG
            debug_message("MINER: Invalid Difficulty\n");
            #endif
            return 0;
    }

    return 0;
}

/* Function to verify a nonce */
int verify_nonce(const Block *block, int transactions_per_block) {
    #if DEBUG
    debug_message("POW: Starting verification of nonce %d\n", block->nonce);
    #endif  
    char hash[HASH_SIZE];
    int reward = get_max_transaction_reward(block, transactions_per_block);
    #if DEBUG
    debug_message("POW: Reward: %d\n", reward);
    #endif
    compute_sha256(block, hash, transactions_per_block);
    #if DEBUG
    debug_message("POW: Hash: %s (nonce: %d)\n", hash, block->nonce);
    #endif
    return check_difficulty(hash, reward);
}

/* Proof-of-Work function */
PoWResult proof_of_work(Block *block, int transactions_per_block) {
    PoWResult result;
    result.elapsed_time = 0.0;
    result.operations = 0;
    result.error = 0;

    block->nonce = 0;
    int reward = get_max_transaction_reward(block, transactions_per_block);

    char hash[HASH_SIZE];
    clock_t start = clock();

    while (1) {
        compute_sha256(block, hash, transactions_per_block);

        if (check_difficulty(hash, reward)) {
            result.elapsed_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            strcpy(result.hash, hash);
            #if DEBUG
            debug_message("MINER: Found valid hash after %d operations\n", result.operations);
            #endif
            return result;
        }

        block->nonce++;
        if (block->nonce > POW_MAX_OPS) {
            #if DEBUG
            debug_message("MINER: Giving up after %d operations\n", POW_MAX_OPS);
            #endif
            result.elapsed_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            result.error = 1;
            return result;
        }

        if (DEBUG && block->nonce % 100000 == 0) {
            #if DEBUG
            debug_message("MINER: Nonce %d\n", block->nonce);
            #endif
        }
        result.operations++;
    }
}

DifficultyLevel getDifficultFromReward(const int reward) {
    if (reward <= 1)
        return EASY;    // 0000[0-b]
    if (reward == 2)
        return NORMAL;  // 00000
    return HARD;       // 00000[0-b] (reward > 2)
}
