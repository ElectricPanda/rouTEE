#include <stdarg.h>
#include <stdio.h>

#include "Enclave.h"
#include "Enclave_t.h"

#include "channel.h"
#include "errors.h"
#include "state.h"
#include "utils.h"

// user address to the user's channels
map<string, vector<Channel*>> addresses_to_channels;

// map[channel_id] = Channel*
map<string, Channel*> channels;

// global state
State state;

// invoke OCall to display the enclave buffer to the terminal screen
void printf(const char* fmt, ...) {

    char buf[BUFSIZ] = {'\0'};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, BUFSIZ, fmt, ap);
    va_end(ap);
    ocall_print_string(buf); // OCall
}

// Ecall: print hello world to the terminal screen
void printf_helloworld() {
    printf("Hello World!\n");
}

int ecall_add_channel(const char* tx_id, int tx_id_len, unsigned int tx_index) {
    // TODO: get the tx from the bitcoin client
    Channel *ch = new Channel;
    ch->addresses[0] = "0xaa";
    ch->addresses[1] = "0xbb";
    ch->balances[0] = 10;
    ch->balances[1] = 20;
    ch->tx_id = string(tx_id, tx_id_len);
    ch->tx_index = tx_index;

    map<string, Channel*>::iterator iter = channels.find(ch->get_id());
    if (iter != channels.end()) {
        // this channel is already added before
        return ERR_ALREADY_EXIST_CHANNEL;
    }
    
    printf("new channel added: %s:%llu / %s:%llu\n", ch->addresses[0], ch->balances[0], ch->addresses[1], ch->balances[1]);
    channels[ch->get_id()] = ch;
    return NO_ERROR;
}

int ecall_remove_channel(const char* target_ch_id, int ch_id_len) {
    string ch_to_remove = string(target_ch_id, ch_id_len);
    map<string, Channel*>::iterator iter = channels.find(ch_to_remove);
    if (iter == channels.end()) {
        // there is no channel whose id is target_ch_id
        return ERR_NO_CHANNEL;
    }
    else {
        // remove the channel
        channels.erase(ch_to_remove);
        return NO_ERROR;
    }
}

void ecall_print_channels() {
    for (map<string, Channel*>::iterator iter = channels.begin(); iter != channels.end(); iter++){
        printf("print channel %s info: %s\n", (iter->second)->get_id().c_str(), (iter->second)->to_string().c_str());
    }
    return;
}

int ecall_do_payment(const char* channel_id, int ch_id_len, const char* sender_address, int address_len, unsigned long long amount) {
    string ch_id = string(channel_id, ch_id_len);
    string sender_addr = string(sender_address, address_len);

    // find the channel
    Channel* ch;
    map<string, Channel*>::iterator iter = channels.find(ch_id);
    if (iter == channels.end()) {
        // there is no channel whose id is target_ch_id
        return ERR_NO_CHANNEL;
    }
    else {
        ch = iter->second;
    }

    // check whether the sender can pay
    if (sender_addr == ch->addresses[0]) {
        // check balance
        if (amount > ch->balances[0]) {
            return ERR_NOT_ENOUGH_BALANCE;
        }

        // do payment
        ch->balances[0] -= amount;
        ch->balances[1] += amount;
    }
    else if (sender_addr == ch->addresses[1]) {
        // check balance
        if (amount > ch->balances[1]) {
            return ERR_NOT_ENOUGH_BALANCE;
        }

        // do payment
        ch->balances[1] -= amount;
        ch->balances[0] += amount;
    }
    else {
        // sender is not in this channel
        return ERR_INVALID_USER;
    }

    return NO_ERROR;
}

unsigned long long ecall_get_channel_balance(const char* channel_id, int ch_id_len, const char* user_address, int address_len) {
    string ch_id = string(channel_id, ch_id_len);
    string user_addr = string(user_address, address_len);

    // find the channel
    Channel* ch;
    map<string, Channel*>::iterator iter = channels.find(ch_id);
    if (iter == channels.end()) {
        // there is no channel whose id is target_ch_id
        // just return max unsigned long long value for easy coding
        return MAX_UNSIGNED_LONG_LONG;
    }
    else {
        ch = iter->second;
    }

    // check whether the sender can pay
    if (user_addr == ch->addresses[0]) {
        return ch->balances[0];
    }
    else if (user_addr == ch->addresses[1]) {
        return ch->balances[1];
    }
    else {
        // sender is not in this channel
        // just return max unsigned long long value for easy coding
        return MAX_UNSIGNED_LONG_LONG;
    }
}

int ecall_set_master(const char* master_address, int master_addr_len){
    // TODO: check authority to set new master address
    // if (cannot set new master) {
    //     return ERR_NO_AUTHORITY;
    // }

    state.master_address = string(master_address, master_addr_len);
    return NO_ERROR;
}

int ecall_set_routing_fee(unsigned long long fee){
    // TODO: check authority to set new routing fee
    // if (cannot set new routing fee) {
    //     return ERR_NO_AUTHORITY;
    // }

    state.routing_fee = fee;
    return NO_ERROR;
}

int ecall_set_routing_fee_address(const char* fee_address, int fee_addr_len){
    // TODO: check authority to set new routing fee address
    // if (cannot set new routing fee address) {
    //     return ERR_NO_AUTHORITY;
    // }

    state.fee_address = string(fee_address, fee_addr_len);
    return NO_ERROR;
}

int ecall_create_channel(const char* tx_id, int tx_id_len, unsigned int tx_index) {
    
    // TODO: compare the tx receiver vs rouTEE master key
    string receiver_addr = "master";
    if (receiver_addr != state.master_address) {
        return ERR_INVALID_RECEIVER;
    }

    // TODO: get sender address & amount from the tx
    string sender_addr = string(tx_id, tx_id_len) + "_" + long_long_to_string(tx_index);
    unsigned long long amount = 100;

    state.user_balances[sender_addr] += amount;
    
    printf("new channel created with rouTEE -> user: %s / %balance:%llu\n", sender_addr.c_str(), amount);
    return NO_ERROR;
}

void ecall_print_state() {
    // print all the state: all users' address and balance
    printf("    master address: %s\n", state.master_address.c_str());
    printf("    routing fee: %llu\n", state.routing_fee);
    printf("    routing fee to %s\n", state.fee_address.c_str());
    for (map<string, unsigned long long>::iterator iter = state.user_balances.begin(); iter != state.user_balances.end(); iter++){
        printf("    user %s balance: %llu\n", (iter->first).c_str(), iter->second);
    }
    return;
}

int ecall_settle_balance(const char* receiver_address, int receiver_addr_len) {
    // TODO: check authority to get paid the balance
    // if (no authority to get balance) {
    //     return ERR_NO_AUTHORITY;
    // }

    // check the receiver has more than 0 balance
    string receiver_addr = string(receiver_address, receiver_addr_len);
    map<string, unsigned long long>::iterator iter = state.user_balances.find(receiver_addr);
    if (iter == state.user_balances.end() || iter->second == 0) {
        // receiver is not in the state || has no balance
        return ERR_NOT_ENOUGH_BALANCE;
    }

    // TODO: make on-chain tx
    // tx = make_settle_tx();
    // ocall_send_tx();

    // remove the user from the state
    printf("user %s get paid %llu satoshi\n", receiver_addr.c_str(), iter->second);
    state.user_balances.erase(receiver_addr);
    return NO_ERROR;
}

void ecall_seal_channels() {
    // https://github.com/intel/linux-sgx/blob/master/SampleCode/SealUnseal/Enclave_Seal/Enclave_Seal.cpp
    // https://github.com/intel/linux-sgx/blob/master/SampleCode/SealUnseal/App/App.cpp
}

void ecall_unseal_channels() {
    
}
