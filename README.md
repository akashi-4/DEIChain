# Blockchain Simulation Project

## Overview

This project simulates a blockchain network with a Controller, Miners, and a Transaction Generator. The Controller manages the overall system, the Miners process transactions, and the Transaction Generator creates transactions.

## Required Components for Intermediate Delivery

### Transaction Generator

- Create the Transaction Generator (TxGen) process
- Implement correct reading of command line parameters (reward 1-3, sleep time 200-3000ms)
- Set up the mechanism to write transactions to shared memory (Transaction Pool)

### Controller

- Implement configuration file reading ("config.cfg")
- Validate the configuration data and apply the read configurations
- Create the Miner, Validator, and Statistics processes
- Set up two shared memory segments (for Transaction Pool and Blockchain Ledger)
- Begin implementation for SIGINT signal capture (preliminary)

### Miner

- Create the specified number of Miner threads according to the configuration file

### Log File

- Implement synchronized output to both log file ("DEIChain_log.txt") and screen

## General Requirements

- Create a makefile for building the project
- Prepare a diagram (1 A4 page) showing architecture and synchronization mechanisms
- Implement preliminary synchronization with suitable mechanisms (semaphores, mutexes or condition variables)

## Data Structures to Implement

You'll need to define at least these data structures:

- Transaction structure (with ID, reward, sender/receiver, value, timestamp)
- Transaction Pool in shared memory (with current_block_id, transactions_pending_set)
- Initial structure for blocks

## Development Approach

- Start by implementing the data structures needed for the system
- Create the Controller process first (reads config, sets up IPC, creates other processes)
- Implement the Transaction Generator next
- Set up the Miner process with its threads
- Implement basic logging functionality
- Add preliminary synchronization mechanisms
