#include <stdarg.h>
#include <stdio.h>

#include "routee.h"
#include "routee_t.h"
#include "sgx_trts.h"
#include "sgx_tseal.h"

#include "channel.h"
#include "errors.h"
#include "state.h"
#include "utils.h"
#include "network.h"

#include <univalue.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <random.h>
#include "rpc.h"
#include "core_io.h"

#include "mbedtls/sha256.h"
#include "bitcoin/key.h"

#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/rsa.h"
#include "mbedtls/error.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
#include "mbedtls/pem.h"
#include "mbedtls/ctr_drbg.h"

#include <stdlib.h>
#include <string.h>


// print ecall results on screen or not
const bool doPrint = true;

// for AES-GCM128 encription / decription
#define SGX_AESGCM_MAC_SIZE 16 // bytes
#define SGX_AESGCM_IV_SIZE 12 // bytes
#define BUFLEN 2048

// bitcoin Pay-to-PubkeyHash tx size info (approximately, tx size = input_num * input_size + output_num * output_size)
#define TX_INPUT_SIZE 150 // bytes
#define TX_OUTPUT_SIZE 40 // bytes

// tax rate to make settle tx (1.1 means 10%)
#define TAX_RATE_FOR_SETTLE_TX 1.1

#include <sgx_thread.h>
sgx_thread_mutex_t state_mutex = SGX_THREAD_MUTEX_INITIALIZER;

#define mbedtls_printf          printf
#define mbedtls_exit            exit
#define MBEDTLS_EXIT_SUCCESS    EXIT_SUCCESS
#define MBEDTLS_EXIT_FAILURE    EXIT_FAILURE

// global state
State state;

// Globals for teechain enclave
bool testnet = false;
bool regtest = true;
bool debug = false;
bool benchmark = false;


// invoke OCall to display the enclave buffer to the terminal screen
void printf(const char* fmt, ...) {

    char buf[BUFSIZ] = {'\0'};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, BUFSIZ, fmt, ap);
    va_end(ap);
    ocall_print_string(buf); // OCall
}

int verify_user(const char* command, int cmd_len, const char* signature, int sig_len, string sessionID) {
    // hard-coded for alice's public key
    // char *_input = "MHQCAQEEIBw8tZ8KWVRPaBNFfaFp4sTeLLJGxdo5QIV4wQgdTZLWoAcGBSuBBAAKoUQDQgAER8COHjvsBtdXSL33gWCLqK0nXSpeAT5s3UIaG+QaknZvE3Bw0R+JluoY1RTROr4yzq2T2hHxkNOPYFTsVxCneA==";
    // unsigned char *input = reinterpret_cast<unsigned char *>(_input);
    // unsigned char pubkey[118];
    // size_t pubkeylen;

    // printf("strlen_input: %d\n\n", strlen(_input));

    // ret = mbedtls_base64_decode( NULL, 0, &pubkeylen, input, strlen(_input) );
    // if ( ret == MBEDTLS_ERR_BASE64_INVALID_CHARACTER )
    //     printf("MBEDTLS_ERR_BASE64_INVALID_CHARACTER\n\n");

    // printf("pubkeylen: %d\n\n", pubkeylen);
    if (sessionID.compare("host") == 0) {
        return 0;
    }

    unsigned char* pubkey = (unsigned char*) state.verify_keys[sessionID].c_str();
    size_t pubkey_len = state.verify_keys[sessionID].length();
    
    unsigned char hash[32];
    int ret = 1;

    mbedtls_ecdsa_context ctx_verify;
    mbedtls_mpi r, s;

    mbedtls_ecdsa_init( &ctx_verify );
    mbedtls_mpi_init( &r );
    mbedtls_mpi_init( &s );

    if ((ret = mbedtls_ecp_group_load( &ctx_verify.grp, MBEDTLS_ECP_DP_SECP256K1 )) != 0) {
        printf("ecp_group_load ret: %d\n\n", ret);
        goto exit;
    }

    if ((ret = mbedtls_ecp_point_read_binary( &ctx_verify.grp, &ctx_verify.Q,
                           pubkey, pubkey_len )) != 0) {
        printf("ecp_point_read_binary ret: %x\n\n", ret);
        goto exit;               
    }

    if ((ret = mbedtls_ecp_check_pubkey( &ctx_verify.grp, &ctx_verify.Q )) != 0) {
        printf("ecp_check_pubkey ret: %x\n\n", ret);
        goto exit;
    }
    /*
     * Compute message hash
     */
    mbedtls_sha256( (unsigned char*) command, cmd_len, hash, 0 );

    /*
     * Verify signature
     */
    if ((ret = mbedtls_mpi_read_binary( &r, (unsigned char*) signature, 32 )) != 0) {
        printf("mpi_read_binary r ret: %x\n", ret);
        goto exit;
    }
    if ((ret = mbedtls_mpi_read_binary( &s, (unsigned char*) signature + 32, 32 )) != 0) {
        printf("mpi_read_binary s ret: %x\n", ret);
        goto exit;
    }

    if ((ret = mbedtls_ecdsa_verify( &ctx_verify.grp,
                  hash, sizeof(hash),
                  &ctx_verify.Q, &r, &s)) != 0) {
        printf("ecdsa_verify ret: %x\n\n", ret);
        goto exit;
    }

exit:
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecdsa_free( &ctx_verify );

    return ret;
}

int ecall_set_routing_fee(const char* command, int cmd_len, const char* signature, int sig_len){
    //
    // TODO: BITCOIN
    // check authority to set new routing fee (ex. rouTEE host's signature)
    if (verify_user(command, cmd_len, signature, sig_len, "host") != 0) {
        return ERR_NO_AUTHORITY;
    }

    char* _cmd = strtok((char*) command, " ");
    char* routing_fee = strtok(NULL, " ");

    if (routing_fee == NULL) {
        printf("No parameter for routing fee\n");
        return ERR_INVALID_PARAMS;
    }

    // set routing fee
    state.routing_fee = strtoull(routing_fee, NULL, 10);
    
    // print result
    if (doPrint) {
        printf("set routing fee as %llu satoshi\n", state.routing_fee);
    }

    return NO_ERROR;
}

int ecall_set_routing_fee_address(const char* command, int cmd_len, const char* signature, int sig_len){
    //
    // TODO: BITCOIN
    // check authority to set new routing fee (ex. rouTEE host's signature)

    printf("command: %s, %d\n", command, strlen(command));
    if (verify_user(command, cmd_len, signature, sig_len, "host") != 0) {
        return ERR_NO_AUTHORITY;
    }

    char* _cmd = strtok((char*) command, " ");
    char* _fee_address = strtok(NULL, " ");

    if (_fee_address == NULL) {
        printf("No parameter for routing fee address\n");
        return ERR_INVALID_PARAMS;
    }

    string fee_address = string(_fee_address, BITCOIN_ADDRESS_LEN);
    // SelectParams(CBaseChainParams::REGTEST);
    if (!CBitcoinAddress(fee_address).IsValid()) {

        return ERR_INVALID_PARAMS;
    }

    // set routing fee address
    state.fee_address = fee_address;

    // print result
    if (doPrint) {
        printf("set routing fee address as %s\n", state.fee_address.c_str());
    }

    return NO_ERROR;
}

int ecall_settle_routing_fee(const char* command, int cmd_len, const char* signature, int sig_len) {
    //
    // TODO: BITCOIN
    // check authority to set new routing fee (ex. rouTEE host's signature)
    if (verify_user(command, cmd_len, signature, sig_len, "host") != 0) {
        return ERR_NO_AUTHORITY;
    }

    char* _cmd = strtok((char*) command, " ");
    char* _amount = strtok(NULL, " ");

    if (_amount == NULL) {
        printf("No parameter for settle amount\n");
        return ERR_INVALID_PARAMS;
    }

    // get settle amount
    unsigned long long amount = strtoull(_amount, NULL, 10);
    
    // amount should be bigger than minimum settle amount
    // minimum settle amount = tax to make settlement tx with only 1 settle request user = maximum tax to make settle tx
    unsigned long long minimun_settle_amount = (TX_INPUT_SIZE + TX_OUTPUT_SIZE) * state.avg_tx_fee_per_byte * TAX_RATE_FOR_SETTLE_TX;
    if (amount <= minimun_settle_amount) {
        // printf("too low amount -> minimun_settle_amount: %llu\n", minimun_settle_amount);
        return ERR_TOO_LOW_AMOUNT_TO_SETTLE;
    }

    // check there is enough routing fee to settle
    if (amount > state.routing_fee_confirmed) {
        return ERR_NOT_ENOUGH_BALANCE;
    }

    // push new waiting settle request
    SettleRequest* sr = new SettleRequest;
    sr->address = state.fee_address;
    sr->amount = amount;
    state.settle_requests_waiting.push(sr);

    // set user's account
    state.routing_fee_confirmed -= amount;
    state.routing_fee_settled += amount;

    // increase state id
    state.stateID++;

    // print result
    if (doPrint) {
        printf("user %s requests settlement: %llu satoshi\n", state.fee_address.c_str(), amount);
    }

    return NO_ERROR;
}


void ecall_print_state() {
    // print all the state: all users' address and balance
    // printf("\n\n\n\n\n\n\n\n\n\n******************** START PRINT STATE ********************\n");

    printf("\n\n\n***** owner info *****\n\n");
    printf("owner address: %s\n", state.owner_address.c_str());

    printf("\n\n\n\n\n***** user account info *****\n\n");
    for (map<string, Account*>::iterator iter = state.users.begin(); iter != state.users.end(); iter++){
        printf("address: %s -> balance: %llu / nonce: %llu / min_requested_block_number: %llu / latest_SPV_block_number: %llu\n", 
            (iter->first).c_str(), iter->second->balance, iter->second->nonce, iter->second->min_requested_block_number, iter->second->latest_SPV_block_number);
    }
    // printf("\n=> total %d accounts / total %llu satoshi\n", state.users.size(), state.total_balances);

    // printf("\n\n\n\n\n***** deposit requests *****\n\n");
    for (map<string, DepositRequest*>::iterator iter = state.deposit_requests.begin(); iter != state.deposit_requests.end(); iter++){
        printf("manager address: %s -> sender address: %s / settle_address: %s / block number:%llu\n", 
            (iter->first).c_str(), iter->second->sender_address.c_str(), iter->second->settle_address.c_str(), iter->second->block_number);
    }

    printf("\n\n\n\n\n***** deposits *****\n\n");
    int queue_size = state.deposits.size();
    for (int i = 0; i< queue_size; i++) {
        Deposit* deposit = state.deposits.front();
        // printf("deposit %d: txhash: %s / txindex: %d\n", i, deposit.tx_hash, deposit.tx_index);
        state.deposits.pop();
        state.deposits.push(deposit);
    }

    // printf("\n\n\n\n\n***** waiting settle requests *****\n\n");
    queue_size = state.settle_requests_waiting.size();
    for (int i = 0; i < queue_size; i++) {
        SettleRequest* sr = state.settle_requests_waiting.front();
        // printf("user address: %s / amount: %llu satoshi\n", sr.address, sr.amount);

        // to iterate queue elements
        state.settle_requests_waiting.pop();
        state.settle_requests_waiting.push(sr);
    }

    // printf("\n\n\n\n\n***** pending settle requests *****\n\n");
    queue_size = state.pending_settle_tx_infos.size();
    unsigned long long pending_routing_fees = 0;
    for (int i = 0; i < queue_size; i++) {
        PendingSettleTxInfo* psti = state.pending_settle_tx_infos.front();
        // printf("pending settle tx %d: pending routing fee: %llu satoshi\n", i, psti->pending_routing_fees);
        pending_routing_fees += psti->pending_routing_fees;
        int deposits_size = psti->used_deposits.size();
        for (int j = 0; j < deposits_size; j++) {
            Deposit* deposit = psti->used_deposits.front();
            // printf("    used deposit %d: txhash: %s / txindex: %d\n", j, deposit.tx_hash, deposit.tx_index);
            psti->used_deposits.pop();
            psti->used_deposits.push(deposit);
        }
        int settle_requests_size = psti->pending_settle_requests.size();
        for (int j = 0; j < settle_requests_size; j++) {
            SettleRequest* sr = psti->pending_settle_requests.front();
            // printf("    user address: %s / settle amount: %llu satoshi\n", sr.address, sr.amount);

            // to iterate queue elements
            psti->pending_settle_requests.pop();
            psti->pending_settle_requests.push(sr);
        }
        // printf("\n");

        // to iterate queue elements
        state.pending_settle_tx_infos.pop();
        state.pending_settle_tx_infos.push(psti);
    }

    printf("\n\n\n\n\n***** routing fees *****\n\n");
    printf("routing fee per payment: %llu satoshi\n", state.routing_fee);
    printf("routing fee address: %s\n", state.fee_address.c_str());
    printf("waiting routing fees: %llu satoshi\n", state.routing_fee_waiting);
    printf("pending routing fees: %llu satoshi\n", pending_routing_fees);
    printf("confirmed routing fees: %llu satoshi\n", state.routing_fee_confirmed);
    printf("settled routing fees = %llu\n", state.routing_fee_settled);

    printf("\n\n\n\n\n***** check correctness *****\n\n");
    bool isCorrect = true;
    // printf("d_total_deposit = %llu\n\n", state.d_total_deposit);
    // printf("total_balances = %llu\n", state.total_balances);
    // printf("d_total_settle_amount = %llu\n", state.d_total_settle_amount);
    // printf("d_total_balances_for_settle_tx_fee = %llu\n", state.d_total_balances_for_settle_tx_fee);
    // printf("routing_fee_waiting = %llu\n", state.routing_fee_waiting);
    // printf("pending_routing_fees = %llu\n", pending_routing_fees);
    // printf("routing_fee_confirmed = %llu\n", state.routing_fee_confirmed);
    // printf("routing_fee_settled = %llu\n", state.routing_fee_settled);

    unsigned long long calculated_total_deposit = 0;
    calculated_total_deposit += state.total_balances;
    calculated_total_deposit += state.d_total_settle_amount;
    calculated_total_deposit += state.d_total_balances_for_settle_tx_fee;
    calculated_total_deposit += state.routing_fee_waiting;
    calculated_total_deposit += pending_routing_fees;
    calculated_total_deposit += state.routing_fee_confirmed;
    calculated_total_deposit += state.routing_fee_settled;
    if (state.d_total_deposit != calculated_total_deposit) {
        // printf("\n=> ERROR: total deposit is not correct, some balances are missed\n\n");
        isCorrect = false;
    }
    // printf("\n");

    // printf("d_total_balances_for_settle_tx_fee = %llu\n\n", state.d_total_balances_for_settle_tx_fee);
    // printf("balances_for_settle_tx_fee = %llu\n", state.balances_for_settle_tx_fee);
    // printf("d_total_settle_tx_fee = %llu\n", state.d_total_settle_tx_fee);
    unsigned long long calculated_total_balances_for_settle_tx_fee = 0;
    calculated_total_balances_for_settle_tx_fee += state.balances_for_settle_tx_fee;
    calculated_total_balances_for_settle_tx_fee += state.d_total_settle_tx_fee;
    if (state.d_total_balances_for_settle_tx_fee != calculated_total_balances_for_settle_tx_fee) {
        // printf("\n=> ERROR: total balance for settle tx fee is not correct, some balances are missed\n\n");
        isCorrect = false;
    }
    if (isCorrect) {
        // printf("\n=> CORRECT: all deposits are in correct way\n\n");
    }
    // printf("\n\n\n******************** END PRINT STATE ********************\n");
    return;
}

int verify_pubkey(const char* pubkey, int pubkey_len) {
    mbedtls_ecdsa_context ctx_verify;
    mbedtls_mpi r, s;
    mbedtls_ecdsa_init(&ctx_verify);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = 0;

    if ((ret = mbedtls_ecp_group_load( &ctx_verify.grp, MBEDTLS_ECP_DP_SECP256K1 )) != 0) {
        printf("ecp_group_load ret: %x\n\n", ret);
        goto exit;
    }

    if ((ret = mbedtls_ecp_point_read_binary( &ctx_verify.grp, &ctx_verify.Q,
                        (unsigned char*) pubkey, pubkey_len )) != 0) {
        printf("ecp_point_read_binary ret: %x\n\n", ret);
        goto exit;
    }

    if ((ret = mbedtls_ecp_check_pubkey( &ctx_verify.grp, &ctx_verify.Q )) != 0) {
        printf("ecp_check_pubkey ret: %x\n\n", ret);
        goto exit;
    }

exit:
    mbedtls_ecdsa_free(&ctx_verify);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);

    return ret;
}

// operation function for secure_command
int secure_get_ready_for_deposit(const char* command, int command_len, const char* sessionID, int sessionID_len, const char* _user_pubkey, int pubkey_len, const char* response_msg) {

    CPubKey user_pubkey = CPubKey(_user_pubkey, _user_pubkey + pubkey_len);

    if (!user_pubkey.IsValid()) {
        printf("Invalid public key\n");
        return ERR_INVALID_PARAMS;
    }

    char* _cmd = strtok((char*) command, " ");
    char* _sender_address = strtok(NULL, " ");
    if (_sender_address == NULL) {
        printf("No sender address\n");
        return ERR_INVALID_PARAMS;
    }
    
    char* _settle_address = strtok(NULL, " ");
    if (_settle_address == NULL) {
        printf("No settle address\n");
        return ERR_INVALID_PARAMS;
    }

    string sender_address(_sender_address, BITCOIN_ADDRESS_LEN);
    string settle_address(_settle_address, BITCOIN_ADDRESS_LEN);

    if (!CBitcoinAddress(sender_address).IsValid()) {
        printf("Invalid sender address\n");
        return ERR_INVALID_PARAMS;
    }

    if (!CBitcoinAddress(settle_address).IsValid()) {
        printf("Invalid settle address\n");
        return ERR_INVALID_PARAMS;
    }



    printf("secure_get_ready_for_deposit: %s, %s\n\n", sender_address.c_str(), settle_address.c_str());

    // initialize ECC State for Bitcoin Library
    initializeECCState();
    // generate and print bitcoin addresses to be paid into by the user
    // generate new bitcoin pub/private key and address
    CKey key;
    key.MakeNewKey(true /* compressed */);
    CPubKey pubkey = key.GetPubKey();

    CKeyID keyid = pubkey.GetID();
    CTxDestination* dest = new CTxDestination;
    dest->class_type = 2;
    dest->keyID = &keyid;
    CScript script = GetScriptForDestination(*dest);

    // get redeem script
    std::string script_asm = ScriptToAsmStr(script);

    // TODO: clean up using the bitcoin core code! For now this works as we hardcode the redeem scripts...
    std::string redeem_script;
    if (debug) {
        redeem_script = "76a914c0cbe7ba8f82ef38aed886fba742942a9893497788ac"; // hard coded for tests!
    } else {
        std::string hash_string = script_asm.substr(18, 40); // 18 is offset of hash in asm, 40 is length of RIPEMD160 in hex
        redeem_script = "76a914" + hash_string + "88ac";  // the P2PKH script format
    }

    CBitcoinAddress address;
    address.Set(pubkey.GetID());

    std::string generated_address = address.ToString();
    std::string generated_public_key = HexStr(key.GetPubKey());
    std::string generated_private_key = CBitcoinSecret(key).ToString();

    // get latest block in rouTEE
    // temp code
    unsigned long long latest_block_number = state.block_number;

    DepositRequest *deposit_request = new DepositRequest;
    deposit_request->manager_private_key = generated_private_key;
    deposit_request->sender_address = sender_address;
    deposit_request->settle_address = settle_address;
    deposit_request->block_number = latest_block_number;

    state.verify_keys[string(sessionID, sessionID_len)] = string(_user_pubkey, pubkey_len);
    
    state.deposit_requests[keyid.ToString()] = deposit_request;
    
    // print result
    if (doPrint) {
        printf("random manager address: %s / block number: %llu\n", generated_address.c_str(), latest_block_number);
    }

    // send random address & block info to the sender
    response_msg = (generated_address + " " + long_long_to_string(latest_block_number)).c_str();

    return NO_ERROR;
}

// operation function for secure_command
int secure_settle_balance(const char* command, int cmd_len, const char* sessionID, int sessionID_len, const char* signature, int sig_len, const char* response_msg) {
    //
    // TODO: BITCOIN
    // check authority to get paid the balance (ex. user's signature with settlement params)
    char* _cmd = strtok((char*) command, " ");
    char* _user_address = strtok(NULL, " ");

    if (_user_address == NULL) {
        printf("No user address for settle balance\n");
        return ERR_INVALID_PARAMS;
    }
    
    char* _amount = strtok(NULL, " ");
    if (_amount == NULL) {
        printf("No amount parameter for settle balance\n");
        return ERR_INVALID_PARAMS;
    }

    string user_address(_user_address, BITCOIN_ADDRESS_LEN);

    if (!CBitcoinAddress(user_address).IsValid()) {
        printf("Invalid user address for settle balance\n");
        return ERR_INVALID_PARAMS;
    }

    unsigned long long amount = strtoull(_amount, NULL, 10);

    if (verify_user(command, cmd_len, signature, sig_len, string(sessionID, sessionID_len)) != 0) {
        return ERR_NO_AUTHORITY;
    }

    // amount should be bigger than minimum settle amount
    // minimum settle amount = tax to make settlement tx with only 1 settle request user = maximum tax to make settle tx
    unsigned long long minimun_settle_amount = (TX_INPUT_SIZE + TX_OUTPUT_SIZE) * state.avg_tx_fee_per_byte * TAX_RATE_FOR_SETTLE_TX;
    if (amount <= minimun_settle_amount) {
        // printf("too low amount -> minimun_settle_amount: %llu\n", minimun_settle_amount);
        return ERR_TOO_LOW_AMOUNT_TO_SETTLE;
    }

    // check the user has enough balance
    map<string, Account*>::iterator iter = state.users.find(user_address);
    if (iter == state.users.end() || iter->second->balance < amount) {
        // user is not in the state || has not enough balance
        return ERR_NOT_ENOUGH_BALANCE;
    }
    Account* user_acc = iter->second;

    // push new waiting settle request
    SettleRequest* sr = new SettleRequest;
    sr->address = user_address;
    sr->amount = amount;
    state.settle_requests_waiting.push(sr);

    // set user's account
    user_acc->balance -= amount;
    user_acc->nonce++; // prevent payment replay attack    

    // update user's requested_block_number
    if (user_acc->balance == 0) {
        // user settled all balance -> reset min requested block number
        user_acc->min_requested_block_number = 0;
    }

    // update total balances
    state.total_balances -= amount;

    // increase state id
    state.stateID++;

    // for debugging
    state.d_total_settle_amount += amount;

    // print result
    if (doPrint) {
        printf("user %s requested settlement: %llu satoshi\n", user_address.c_str(), amount);
    }

    return NO_ERROR;
}

static void fill_inputs(string& inputs, unsigned int index_to_start_at) {

    PendingSettleTxInfo* psti = state.pending_settle_tx_infos.back();

    unsigned int deposits_size = psti->used_deposits.size();

    for (unsigned int i = 0; i < deposits_size; i++) {
        Deposit* deposit = psti->used_deposits.front();

        std::string tx_hash = deposit->tx_hash;
        unsigned long long tx_index = deposit->tx_index;

        if ((index_to_start_at != 0) || (i != 0)) {
            inputs += ","; // add commas if not first item
        }

        inputs += "{\"txid\":\"" + tx_hash + "\",\"vout\":" + long_long_to_string(tx_index) + "}";
    }
}

int ecall_make_settle_transaction(const char* settle_transaction, int* settle_tx_len) {

    // 
    // TODO: check rouTEE is ready to settle
    // ex. check there is no pending settle tx || at least 1 user requested settlement
    //
    if (state.pending_settle_tx_infos.size() != 0 || state.settle_requests_waiting.size() == 0) {
        return ERR_SETTLE_NOT_READY;
    }

    // 
    // TODO: BITCOIN
    // ex. unsigned long long routing_fees_to_be_confirmed = make_settle_tx(settle_transaction, settle_tx_len);
    // returns (routing_fees_pending * total_settle_amount / total_balances)
    // and fill in settle_transaction and settle_tx_len
    // and move SettleRequest from state.settle_requests_waiting to state.settle_requests_pending
    // and state.settle_tx_hashes_pending.push(settle_tx_hash)
    // 
    // temp code
    // save infos of this settle tx
    PendingSettleTxInfo* psti = new PendingSettleTxInfo;
    psti->tx_hash = "0x_settle_tx_hash";
    psti->pending_balances = 0;
    int tx_input_num = state.deposits.size();
    int settle_users_num = state.settle_requests_waiting.size();
    int tx_output_num = settle_users_num;
    unsigned long long balance_for_settle_tx_fee = (tx_output_num * TX_OUTPUT_SIZE) / settle_users_num * state.avg_tx_fee_per_byte * TAX_RATE_FOR_SETTLE_TX;
    if (state.total_balances != 0 || state.routing_fee_waiting != 0 || state.routing_fee_confirmed != 0) {
        tx_output_num += 1; // +1 means leftover_deposit
        balance_for_settle_tx_fee = ((tx_output_num * TX_OUTPUT_SIZE) + TX_INPUT_SIZE) / settle_users_num * state.avg_tx_fee_per_byte * TAX_RATE_FOR_SETTLE_TX;
    }
    else {
        // so just give balances_for_settle_tx_fee to state.fee_address
        unsigned long long bonus = state.balances_for_settle_tx_fee;
        state.settle_requests_waiting.front()->amount += bonus;
        state.d_total_balances_for_settle_tx_fee -= bonus;
        state.routing_fee_settled += bonus;
        state.balances_for_settle_tx_fee = 0;
        balance_for_settle_tx_fee = (TX_INPUT_SIZE + TX_OUTPUT_SIZE) * state.avg_tx_fee_per_byte;

        // print result
        if (doPrint) {
            // there is no user balance left, no routing fee left
            // this means this settle tx is to settle all routing_fee_confirmed alone
            // printf("there is nothing left to settle. this settle tx cleans all the things.\n");
            // printf("bonus for clean-up settle tx: give %llu satoshi to fee address\n", bonus);
        }
    }
    
    while(!state.settle_requests_waiting.empty()) {
        SettleRequest* sr = state.settle_requests_waiting.front();
        psti->pending_balances += sr->amount;
        if (doPrint) {
            // printf("settle tx output: to %s / %llu satoshi\n", sr.address.c_str(), sr.amount);
        }

        // change settle requests status: from waiting to pending
        // & calculate settlement tax
        state.settle_requests_waiting.pop();
        sr->balance_for_settle_tx_fee = balance_for_settle_tx_fee;
        state.balances_for_settle_tx_fee += sr->balance_for_settle_tx_fee;
        state.d_total_balances_for_settle_tx_fee += sr->balance_for_settle_tx_fee;
        state.d_total_settle_amount -= sr->balance_for_settle_tx_fee;
        psti->pending_settle_requests.push(sr);
    }
    while (!state.deposits.empty()) {
        // move deposits: from unused to used
        Deposit* deposit = state.deposits.front();
        state.deposits.pop();
        psti->used_deposits.push(deposit);
    }
    psti->pending_routing_fees = state.routing_fee_waiting * psti->pending_balances / (state.total_balances + psti->pending_balances);
    state.routing_fee_waiting -= psti->pending_routing_fees;
    int settle_tx_size = TX_INPUT_SIZE * tx_input_num + TX_OUTPUT_SIZE * tx_output_num;
    psti->pending_tx_fee = state.avg_tx_fee_per_byte * settle_tx_size;
    state.pending_settle_tx_infos.push(psti);
    state.balances_for_settle_tx_fee -= psti->pending_tx_fee;
    Deposit* leftover_deposit = new Deposit;
    state.deposits.push(leftover_deposit);
    psti->leftover_deposit = leftover_deposit;

    // for debugging
    state.d_total_settle_tx_fee += psti->pending_tx_fee;

    // print result
    if (doPrint) {
        printf("settle tx intput num: %d / settle tx output num: %d\n", tx_input_num, tx_output_num);
        printf("routing fee waiting: %llu / psti->pending balances: %llu / state.total balance: %llu\n", state.routing_fee_waiting, psti->pending_balances, state.total_balances);
    }


    // std::string input_string = "["; // open list of inputs
    // std::string output_string = "{"; // open outputs

    // // fill my inputs
    // fill_inputs(&input_string, 0 /* index to start at */);

    // // Calculate miner fee using the given miner fee per byte value
    // //miner_fee = calculate_total_fee(miner_fee, my_deposit_ids.size() + remote_deposits.size(), 2);

    // // We pay the miner fee because we want to generate the transaction!
    // if (my_balance <= miner_fee) {
    //     printf("Warning! The amount of money you have remaining in the channel "
    //            "is less than the miner fee to pay! We are generating a transaction for you, "
    //            "but it won't have a miner fee!");
    //     append_output_to_settlement_transaction(&output_string, my_address, remote_address, my_balance, remote_balance, 0);
    // } else {
    //     append_output_to_settlement_transaction(&output_string, my_address, remote_address, my_balance, remote_balance, miner_fee);
    // }

    //      append_output_to_settlement_transaction(&output_string, psti->pending_tx_fee);

    // input_string += "]"; // close list of inputs
    // output_string += "}"; // close outputs

    // std::string create_transaction_rpc = create_raw_transaction_rpc();
    // create_transaction_rpc += input_string + " " + output_string;
    // UniValue settle_transaction = executeCommand(create_transaction_rpc);
    // std::string settle_transaction_string = remove_surrounding_quotes(settle_transaction.write());
    // return settle_transaction_string;


    return NO_ERROR;
}

// operation function for secure_command
int secure_do_multihop_payment(const char* command, int cmd_len, const char* sessionID, int sessionID_len, const char* signature, int sig_len, const char* response_msg) {
    //
    // TODO: BITCOIN
    // check authority to send (ex. sender's signature with these params)
    char* _cmd = strtok((char*) command, " ");
    char* _sender_address = strtok(NULL, " ");
    if (_sender_address == NULL) {
        printf("No sender address for multihop payment\n");
        return ERR_INVALID_PARAMS;
    }

    char* _receiver_address = strtok(NULL, " ");
    if (_receiver_address == NULL) {
        printf("No receiver address for multihop payment\n");
        return ERR_INVALID_PARAMS;
    }
    
    char* _amount = strtok(NULL, " ");
    if (_amount == NULL) {
        printf("No amount parameter for multihop payment\n");
        return ERR_INVALID_PARAMS;
    }

    char* _fee = strtok(NULL, " ");
    if (_fee == NULL) {
        printf("No routing fee parameter for multihop payment\n");
        return ERR_INVALID_PARAMS;
    }

    string sender_address(_sender_address, BITCOIN_ADDRESS_LEN);
    string receiver_address(_receiver_address, BITCOIN_ADDRESS_LEN);
    unsigned long long amount = strtoull(_amount, NULL, 10);
    unsigned long long fee = strtoull(_fee, NULL, 10);

    if (!CBitcoinAddress(sender_address).IsValid()) {
        printf("Invalid sender address for multihop payment\n");
        return ERR_INVALID_PARAMS;
    }

    if (!CBitcoinAddress(receiver_address).IsValid()) {
        printf("Invalid receiver address for multihop payment\n");
        return ERR_INVALID_PARAMS;
    }

    if (verify_user(command, cmd_len, signature, sig_len, string(sessionID, sessionID_len)) != 0) {
        return ERR_NO_AUTHORITY;
    }

    // check the sender exists & has more than amount + fee to send
    map<string, Account*>::iterator iter = state.users.find(sender_address);
    if (iter == state.users.end() || iter->second->balance < amount + fee) {
        // sender is not in the state || has not enough balance
        return ERR_NOT_ENOUGH_BALANCE;
    }
    Account* sender_acc = iter->second;

    // check routing fee
    if (fee < state.routing_fee) {
        return ERR_NOT_ENOUGH_FEE;
    }

    // check the receiver exists
    iter = state.users.find(receiver_address);
    if (iter == state.users.end()) {
        // receiver is not in the state
        return ERR_NO_RECEIVER;
    }
    Account* receiver_acc = iter->second;

    // check the receiver is ready to get paid (temporarily deprecated for easy tests)
    // if (sender_acc->min_requested_block_number > receiver_acc->latest_SPV_block_number) {
    //     return ERR_RECEIVER_NOT_READY;
    // }

    // move balance
    sender_acc->balance -= (amount + fee);
    receiver_acc->balance += amount;

    // add routing fee for this payment
    state.routing_fee_waiting += fee;
    // update total balances
    state.total_balances -= fee;

    // increase sender's nonce
    sender_acc->nonce++;

    // update receiver's requested_block_number
    if (receiver_acc->min_requested_block_number < sender_acc->min_requested_block_number) {
        receiver_acc->min_requested_block_number = sender_acc->min_requested_block_number;
    }

    // update sender's requested_block_number
    if (sender_acc->balance == 0) {
        // sender spent all balance -> reset min requested block number
        sender_acc->min_requested_block_number = 0;
    }

    // increase state id
    state.stateID++;

    // print result
    if (doPrint) {
        // printf("user %s send %llu satoshi to user %s (routing fee: %llu)\n", sender_address.c_str(), amount, receiver_address.c_str(), fee);
    }

    return NO_ERROR;
}

// update user's latest SPV block
int secure_update_latest_SPV_block(const char* command, int cmd_len, const char* sessionID, int sessionID_len, const char* signature, int sig_len, const char* response_msg) {
    //
    // TODO: BITCOIN
    // check authority to change SPV block
    // ex. verify user's signature
    //
    char* _cmd = strtok((char*) command, " ");
    char* _user_address = strtok(NULL, " ");
    if (_user_address == NULL) {
        printf("No user address for update last SPV block\n");
        return ERR_INVALID_PARAMS;
    }

    char* _block_number = strtok(NULL, " ");
    if (_block_number == NULL) {
        printf("No block number for update last SPV block\n");
        return ERR_INVALID_PARAMS;
    }

    string user_address(_user_address, BITCOIN_ADDRESS_LEN);
    unsigned long long block_number = strtoull(_block_number, NULL, 10);

    if (!CBitcoinAddress(user_address).IsValid()) {
        printf("Invalid user address for update last SPV block\n");
        return ERR_INVALID_PARAMS;
    }

    if (verify_user(command, cmd_len, signature, sig_len, string(sessionID, sessionID_len)) != 0) {
        return ERR_NO_AUTHORITY;
    }

    // check the user exists
    map<string, Account*>::iterator iter = state.users.find(user_address);
    if (iter == state.users.end()) {
        // the user not exist
        // printf("address %s is not in the state\n", user_address);
        return ERR_ADDRESS_NOT_EXIST;
    }
    Account* user_acc = iter->second;

    // check user has same block with rouTEE
    // if () {
    //     ;
    // }

    // check the block number is larger than user's previous latest block number
    if (user_acc->latest_SPV_block_number < block_number) {
        // update block number
        user_acc->latest_SPV_block_number = block_number;
    }
    else {
        // cannot change to lower block
        return ERR_CANNOT_CHANGE_TO_LOWER_BLOCK;
    }

    // print result
    if (doPrint) {
        // printf("user %s update SPV block number to %llu\n", user_address.c_str(), block_number);
    }
    
    return NO_ERROR;
}

// this is not ecall function, but this can be used as ecall to debugging
// TODO: do not send sender_address param, change this to manager_address and get deposit infos from state.deposit_requests[manager_address] (do this later for simple experiment)
// void deal_with_deposit_tx(const char* sender_address, int sender_addr_len, unsigned long long amount, unsigned long long block_number) {

//     // will take some of the deposit to pay tx fee later
//     unsigned long long balance_for_tx_fee = state.avg_tx_fee_per_byte * TX_INPUT_SIZE * TAX_RATE_FOR_SETTLE_TX;

//     // will take some of the deposit to induce rouTEE host not to forcely terminate the rouTEE program (= incentive driven agent assumption)
//     // = just simply pay routing fee

//     // check sender sent enough deposit amount
//     unsigned long long minimum_amount_of_deposit = balance_for_tx_fee + state.routing_fee;
//     if (amount <= minimum_amount_of_deposit) {
//         // printf("too low amount of deposit, minimum amount is %llu\n", minimum_amount_of_deposit);
//         return;
//     }

//     // check the user exists
//     string sender_addr = string(sender_address, sender_addr_len);
//     map<string, Account*>::iterator iter = state.users.find(sender_addr);
//     if (iter == state.users.end()) {
//         // sender is not in the state, create new account
//         Account* acc = new Account;
//         acc->balance = 0;
//         acc->nonce = 0;
//         acc->latest_SPV_block_number = 0;
//         acc->settle_address = state.deposit_requests[sender_addr]->settle_address;
//         acc->public_key = state.deposit_requests[sender_addr]->public_key;
//         state.users[sender_addr] = acc;
//     }

//     // now take some of the deposit
//     state.balances_for_settle_tx_fee += balance_for_tx_fee;
//     state.routing_fee_waiting += state.routing_fee;

//     // update user's balance
//     unsigned long long balance_for_user = amount - balance_for_tx_fee - state.routing_fee;
//     state.users[sender_addr]->balance += balance_for_user;

//     // update total balances
//     state.total_balances += balance_for_user;

//     // update user's min_requested_block_number
//     if (balance_for_user > 0) {
//         state.users[sender_addr]->min_requested_block_number = block_number;
//     }

//     // add deposit
//     Deposit* deposit = new Deposit;
//     deposit->tx_hash = "some_tx_hash";
//     deposit->tx_index = 0;
//     deposit->manager_private_key = "0xmanager";
//     state.deposits.push(deposit);

//     // increase state id
//     state.stateID++;

//     // for debugging
//     state.d_total_deposit += amount;
//     state.d_total_balances_for_settle_tx_fee += balance_for_tx_fee;

//     // print result
//     if (doPrint) {
//         // printf("deal with new deposit tx -> user: %s / balance += %llu / tx fee += %llu\n", sender_addr.c_str(), balance_for_user, balance_for_tx_fee);
//     }

//     return;

    // ---------------------------------------------------------------------------------------------------------------------------------------------------

    
    // real implementation for this function
    // change param: sender_address -> manager_address
void deal_with_deposit_tx(const char* manager_address, int manager_addr_len, const char* tx_hash, int tx_hash_len, int tx_index, unsigned long long amount, unsigned long long block_number) {

    // will take some of the deposit to pay tx fee later
    unsigned long long balance_for_tx_fee = state.avg_tx_fee_per_byte * TX_INPUT_SIZE * TAX_RATE_FOR_SETTLE_TX;

    // will take some of the deposit to induce rouTEE host not to forcely terminate the rouTEE program (= incentive driven agent assumption)
    // = just simply pay routing fee

    // check sender sent enough deposit amount
    unsigned long long minimum_amount_of_deposit = balance_for_tx_fee + state.routing_fee;
    if (amount <= minimum_amount_of_deposit) {
        // printf("too low amount of deposit, minimum amount is %llu\n", minimum_amount_of_deposit);
        return;
    }

    // get the deposit request for this deposit tx
    DepositRequest *dr = state.deposit_requests[string(manager_address, manager_addr_len)];

    // check the user exists
    string sender_addr = dr->sender_address;
    map<string, Account*>::iterator iter = state.users.find(sender_addr);
    if (iter == state.users.end()) {
        // sender is not in the state, create new account
        Account* acc = new Account;
        acc->balance = 0;
        acc->nonce = 0;
        acc->latest_SPV_block_number = 0;
        state.users[sender_addr] = acc;
    }

    // update settle address
    state.users[sender_addr]->settle_address = dr->settle_address;

    // now take some of the deposit
    state.balances_for_settle_tx_fee += balance_for_tx_fee;
    state.routing_fee_waiting += state.routing_fee;

    // update user's balance
    unsigned long long balance_for_user = amount - balance_for_tx_fee - state.routing_fee;
    state.users[sender_addr]->balance += balance_for_user;

    // update total balances
    state.total_balances += balance_for_user;

    // update user's min_requested_block_number
    if (balance_for_user > 0) {
        state.users[sender_addr]->min_requested_block_number = block_number;
    }

    // add deposit
    Deposit* deposit = new Deposit;
    deposit->tx_hash = string(tx_hash, tx_hash_len);
    deposit->tx_index = tx_index;
    deposit->manager_private_key = dr->manager_private_key;
    state.deposits.push(deposit);

    // increase state id
    state.stateID++;

    // for debugging
    state.d_total_deposit += amount;
    state.d_total_balances_for_settle_tx_fee += balance_for_tx_fee;

    // print result
    if (doPrint) {
        printf("deal with new deposit tx -> user: %s / balance += %llu / tx fee += %llu\n", dr->sender_address.c_str(), balance_for_user, balance_for_tx_fee);
    }

    return;
    

}

// this is not ecall function, but this can be used as ecall to debugging
void deal_with_settlement_tx() {

    // get this settle tx's info
    // settle txs are included in bitcoin with sequencial order, so just get the first pending settle tx from queue
    PendingSettleTxInfo* psti = state.pending_settle_tx_infos.front();
    state.pending_settle_tx_infos.pop();

    // confirm routing fee for this settle tx
    state.routing_fee_confirmed += psti->pending_routing_fees;

    // print result
    if (doPrint) {
        // printf("deal with settle tx -> rouTEE owner got paid pending routing fee: %llu satoshi\n", psti->pending_routing_fees);

        // dequeue pending settle requests for this settle tx (print for debugging, can delete this later)
        int queue_size = psti->pending_settle_requests.size();
        for (int i = 0; i < queue_size; i++) {
            SettleRequest* sr = psti->pending_settle_requests.front();
            // printf("deal with settle tx -> user: %s / requested %llu satoshi (paid tax: %llu satoshi)\n", sr.address, sr.amount, sr.balance_for_settle_tx_fee);
            psti->pending_settle_requests.pop();
            delete sr;
        }

        // dequeue used deposits for this settle tx (print for debugging, can delete this later)
        queue_size = psti->used_deposits.size();
        for (int i = 0; i < queue_size; i++) {
            Deposit* deposit = psti->used_deposits.front();
            // printf("deal with settle tx -> used deposit hash: %s\n", deposit.tx_hash);
            psti->used_deposits.pop();
            delete deposit;
        }

    }

    return;
}

// bool verify_block(CBlock block){
//     CBlockHeader bh = block.GetBlockHeader();
//     if (!CheckProofOfWork(block.GetHash(), block.nBits)){
//         return false;
//     }
//     if ((bh.nVersion == genesis.nVersion) && (bh.hashMerkleRoot == genesis.hashMerkleRoot) && (bh.nTime == genesis.nTime)
//         && (bh.nNonce == genesis.nNonce) && (bh.nBits == genesis.nBits) && (bh.hashPrevBlock == genesis.hashPrevBlock)){
//         return true;
//     }
//     if(bh.nBits == GetNextWorkRequired(lastIndex, &bh)){
//         CBlockIndex* pindexNew = new CBlockIndex(bh);
//         pindexNew->pprev = lastIndex;
//         pindexNew->nHeight = pindexNew->pprev->nHeight+1;
//         pindexNew->BuildSkip();
//         lastIndex = pindexNew;
//         return true;
//     }
//     return false;
// } 


int ecall_insert_block(int block_number, const char* hex_block, int hex_block_len) {
    // 
    // TODO: BITCOIN
    // SPV verify the new bitcoin block
    // verify tx merkle root hash
    // iterate txs to call deal_with_deposit_tx() when find deposit tx
    //             to call deal_with_settlement_tx() when find settlement tx
    // update average tx fee (state.avg_tx_fee_per_byte)
    // insert the block to state.blocks
    // 
    CBlock block;
    DecodeHexBlk(block, string(hex_block, hex_block_len));

    printf("block info: %s, %d\n\n", block.ToString().c_str(), block.vtx.size());
    printf("tx_vout: %s\n", block.vtx[0]->vout[0].ToString().c_str());
    string txid;
    CTxDestination tx_dest;
    string keyID;
    int tx_index;
    unsigned long long amount;
    string pending_settle_tx_hash;
    
    for (int tx_index = 0; tx_index < block.vtx.size(); tx_index++){
        printf("vtx=: %s\n\n", block.vtx[tx_index]->vout[0].ToString().c_str());
        if (!ExtractDestination(block.vtx[tx_index]->vout[0].scriptPubKey, tx_dest)) {
            printf("Extract Destination Error\n\n");
            printf("rr: %d\n\n", tx_dest.class_type);
        }

        keyID = tx_dest.keyID->ToString();
        txid = block.vtx[tx_index]->GetHash().GetHex();
        amount = block.vtx[tx_index]->vout[0].nValue;
        pending_settle_tx_hash = state.pending_settle_tx_infos.front()->tx_hash;

        if (state.deposit_requests.find(keyID) != state.deposit_requests.end()) {
            printf("deal_with_deposit_tx called\n");
            deal_with_deposit_tx(keyID.c_str(), keyID.length(), txid.c_str(), txid.length(), tx_index, amount, block_number);
        }
        else if (txid.compare(pending_settle_tx_hash) == 0) {
            printf("deal_with_settlement_tx called\n");
            deal_with_settlement_tx();
        }
        
        printf("TX: %s\n", block.vtx[tx_index]->GetHash().GetHex().c_str());
        printf("tx_vout 0: %lld\n", block.vtx[tx_index]->vout[0].nValue);
    }



    // map<string, TxOut*>* txout_list = static_cast<map<string, TxOut*>*>(void_txout_list);
    // TxOut* txout;
    // //string manager_address;
    // unsigned long long amount;
    // string txid;
    // int tx_index;
    // printf("rr, %d\n\n", txout_list->size());
    //for (map<string, TxOut*>::iterator iter_txout = (*txout_list).begin(); iter_txout != (*txout_list).end(); iter_txout++) {
    // for (auto iter_txout = (*txout_list).begin(); iter_txout != (*txout_list).end(); iter_txout++) {
    //     printf("ff, %d, %d, %d\n\n", iter_txout, (*txout_list).begin(), (*txout_list).end());
    //     string manager_address = (*txout_list).begin()->first;
    //     txout = (*txout_list).begin()->second;
    //     printf("manager_address: %s", manager_address.c_str());

    //     if (state.deposit_requests.find(manager_address) != state.deposit_requests.end()) {
    //         amount = txout->amount;
    //         txid = txout->txid;
    //         tx_index = txout->tx_index;
    //         printf("insert_block: %ull", amount);
    //         //deal_with_deposit_tx(manager_address.c_str(), manager_address.length(), txid.c_str(), txid.length(), tx_index, amount, block_number);
    //     }

    //     delete txout;
    // }
    // char* _cmd = strtok((char*) command, " ");
    // char* _block_number = strtok(NULL, " ");
    // if (_block_number == NULL) {
    //     printf("No block number parameter for insert block\n");
    //     return ERR_INVALID_PARAMS;
    // }

    // unsigned block_number = strtoull(_block_number, NULL, 10);

    // for (auto iter_tx = block_txs.begin(); iter_tx != block_txs.end(); iter_tx++) {
    //     string tx_hash = (string) *iter_tx;
    //     for (map<string, DepositRequest*>::iterator iter = state.deposit_requests.begin(); iter != state.deposit_requests.end(); iter++){
    //         string generated_address = iter->first;
    //         DepositRequest* deposit_request = iter->second;

    //         //deal_with_deposit_tx(deposit_request->sender_address, int sender_addr_len, unsigned long long amount, unsigned long long block_number);
    //     }
    // }



    // //for (queue<SettleRequest>::iterator iter = state.settle_requests_waiting.begin(); iter != state.settle_requests_waiting.end(); iter++) {
    //     deal_with_settlement_tx();
    // //}

    printf("block size: %d\n\n", block_number);

    return 0;
}

int make_encrypted_response(const char* response_msg, sgx_aes_gcm_128bit_key_t *session_key, char* encrypted_response, int* encrypted_response_len) {

    // return encrypted response to client
    uint8_t *response = (uint8_t *) response_msg;
    size_t len = strlen(response_msg);
	uint8_t p_dst[BUFLEN] = {0};

	// Generate the IV (nonce)
	sgx_read_rand(p_dst + SGX_AESGCM_MAC_SIZE, SGX_AESGCM_IV_SIZE);

    // encrypt the response
	sgx_status_t status = sgx_rijndael128GCM_encrypt(
		session_key,
		response, len, 
		p_dst + SGX_AESGCM_MAC_SIZE + SGX_AESGCM_IV_SIZE,
		p_dst + SGX_AESGCM_MAC_SIZE,
        SGX_AESGCM_IV_SIZE,
		NULL, 0,
		(sgx_aes_gcm_128bit_tag_t *) (p_dst)
    );

    // check encryption result
    if (status != SGX_SUCCESS) {
        // encryption failed: abnormal case
        // this cannot be happened in ordinary situations
        // SGX_ERROR_INVALID_PARAMETER || SGX_ERROR_OUT_OF_MEMORY || SGX_ERROR_UNEXPECTED
        return ERR_ENCRYPT_FAILED;
    }

    // copy encrypted response to outside buffer
    *encrypted_response_len = SGX_AESGCM_MAC_SIZE+SGX_AESGCM_IV_SIZE+len;
	memcpy(encrypted_response, p_dst, *encrypted_response_len);

    return NO_ERROR;
}

int ecall_secure_command(const char* sessionID, int sessionID_len, const char* encrypted_cmd, int encrypted_cmd_len, char* encrypted_response, int* encrypted_response_len) {

    // error index of this ecall function
    int result_error_index;

    // error index of encryption result
    int encryption_result;

    //
    // decrypt cmd
    //

    uint8_t *encMessage = (uint8_t *) encrypted_cmd;
	uint8_t p_dst[BUFLEN] = {0};
    string session_ID = string(sessionID, sessionID_len);
    
    // sgx_aes_gcm_128bit_key_t *session_key = state.session_keys[session_ID];
    // test code
    sgx_aes_gcm_128bit_key_t skey = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf };
    sgx_aes_gcm_128bit_key_t *session_key = &skey;

    size_t decMessageLen = encrypted_cmd_len - SGX_AESGCM_MAC_SIZE - SGX_AESGCM_IV_SIZE;
	sgx_status_t status = sgx_rijndael128GCM_decrypt(
		session_key,
		encMessage + SGX_AESGCM_MAC_SIZE + SGX_AESGCM_IV_SIZE,
		decMessageLen,
		p_dst,
		encMessage + SGX_AESGCM_MAC_SIZE,
        SGX_AESGCM_IV_SIZE,
		NULL, 0,
		(sgx_aes_gcm_128bit_tag_t *) encMessage
    );

    if (status != SGX_SUCCESS) {
        // return encrypted response to client
        // make encrypted response 
        // and return NO_ERROR to hide the ecall result from rouTEE host
        encryption_result = make_encrypted_response(error_to_msg(ERR_DECRYPT_FAILED), session_key, encrypted_response, encrypted_response_len);
        if (encryption_result != NO_ERROR) {
            // TODO: if encryption failed, send rouTEE's signature for the failed cmd
            // to make client believe that the encrpytion really failed
            return ERR_ENCRYPT_FAILED;
        }
        return NO_ERROR;
    }
    
    char *decMessage = (char *) malloc((decMessageLen+1)*sizeof(char));
	memcpy(decMessage, p_dst, decMessageLen);
    decMessage[decMessageLen] = '\0';

    //
    // execute decrypted cmd
    //
    // @ Luke Park
    // mutex lock
    sgx_thread_mutex_lock(&state_mutex);

    // parse the command to get parameters
    vector<string> params;
    string cmd = string(decMessage, decMessageLen);
    split(cmd, params, ' ');

    const int decSignatureLen = 64;
    const int decCommandLen = decMessageLen - decSignatureLen - 1;

    char *decCommand = (char *) malloc((decCommandLen+1)*sizeof(char));
    memcpy(decCommand, decMessage, decCommandLen);
    decCommand[decCommandLen] = '\0';

    char *decSignature = decMessage + decCommandLen + 1;

    printf("decCommand: %s, %d\n\n", decCommand, decCommandLen);

    // find appropriate operation
    char operation = params[0][0];
    int operation_result;
    const char* response_msg;
    if (operation == OP_GET_READY_FOR_DEPOSIT) {
        operation_result = secure_get_ready_for_deposit(decCommand, decCommandLen - 1, sessionID, sessionID_len, decSignature - 1, decSignatureLen + 1, response_msg);
    }
    else if (operation == OP_SETTLE_BALANCE) {
        operation_result = secure_settle_balance(decCommand, decCommandLen, sessionID, sessionID_len, decSignature, decSignatureLen, response_msg);
    }
    else if (operation == OP_DO_MULTIHOP_PAYMENT) {
        operation_result = secure_do_multihop_payment(decCommand, decCommandLen, sessionID, sessionID_len, decSignature, decSignatureLen, response_msg);
    }
    else if (operation == OP_UPDATE_LATEST_SPV_BLOCK) {
        operation_result = secure_update_latest_SPV_block(decCommand, decCommandLen, sessionID, sessionID_len, decSignature, decSignatureLen, response_msg);
    }
    else {
        // invalid opcode
        operation_result = ERR_INVALID_OP_CODE;
    }

    //
    // encrypt response
    //
    // @ Luke Park
    // mutex unlock
    sgx_thread_mutex_unlock(&state_mutex);

    // encrypt the response for client & return NO_ERROR to hide the ecall result from rouTEE host
    if (operation_result != -1) {
        response_msg = error_to_msg(operation_result);
    }
    encryption_result = make_encrypted_response(response_msg, session_key, encrypted_response, encrypted_response_len);
    if (encryption_result != NO_ERROR) {
        // TODO: if encryption failed, send rouTEE's signature for the failed cmd
        // to make client believe that the encrpytion really failed
        return ERR_ENCRYPT_FAILED;
    }

    // print result
    if (doPrint) {
        // printf("decrypted secure command: %s\n", decMessage);
        // printf("secure command result: %s\n", response_msg);
    }

    return NO_ERROR;
}

int ecall_make_owner_key(char* sealed_owner_private_key, int* sealed_key_len) {
    //
    // TODO: BITCOIN
    // make random bitcoin private key
    //
    // initialize ECC State for Bitcoin Library
    initializeECCState();
    // generate and print bitcoin addresses to be paid into by the user
    // generate new bitcoin pub/private key and address
    CKey key;
    key.MakeNewKey(true /* compressed */);
    CPubKey pubkey = key.GetPubKey();

    CKeyID keyid = pubkey.GetID();

    CBitcoinAddress address;
    address.Set(pubkey.GetID());

    std::string generated_address = address.ToString();
    std::string generated_public_key = HexStr(key.GetPubKey());
    std::string generated_private_key = CBitcoinSecret(key).ToString();

    printf("ecall_make_owner_key.generated_private_key: %s\n", generated_private_key.c_str());
    printf("ecall_make_owner_key.generated_public_key: %s\n", generated_public_key.c_str());
    printf("ecall_make_owner_key.generated_address: %s\n", generated_address.c_str());

    const char *random_private_key = generated_private_key.c_str();
    //char random_private_key[300] = "abcde"; // temp code
    // printf("random private key: %s\n", random_private_key);

    // seal the private key
    uint32_t sealed_data_size = sgx_calc_sealed_data_size(0, (uint32_t)strlen(random_private_key));
    // printf("sealed_data_size: %d\n", sealed_data_size);
    *sealed_key_len = sealed_data_size;
    if (sealed_data_size == UINT32_MAX) {
        return ERR_SGX_ERROR_UNEXPECTED;
    }
    sgx_sealed_data_t *sealed_key_buffer = (sgx_sealed_data_t *) malloc(sealed_data_size);
    sgx_status_t status = sgx_seal_data(0, NULL, (uint32_t)strlen(random_private_key), (uint8_t *) random_private_key, sealed_data_size, sealed_key_buffer);
    if (status != SGX_SUCCESS) {
        return ERR_SEAL_FAILED;
    }

    // copy sealed key to the app buffer
    memcpy(sealed_owner_private_key, sealed_key_buffer, sealed_data_size);
    free(sealed_key_buffer);
    return NO_ERROR;
}

int ecall_load_owner_key(const char* sealed_owner_private_key, int sealed_key_len) {
    // for edge8r
    (void) sealed_key_len;

    // unseal the sealed private key
    uint32_t unsealed_key_length = sgx_get_encrypt_txt_len((const sgx_sealed_data_t *) sealed_owner_private_key);
    uint8_t unsealed_private_key[unsealed_key_length];
    sgx_status_t status = sgx_unseal_data((const sgx_sealed_data_t *) sealed_owner_private_key, NULL, 0, unsealed_private_key, &unsealed_key_length);
    if (status != SGX_SUCCESS) {
        return ERR_UNSEAL_FAILED;
    }

    // set owner_private_key
    state.owner_private_key.assign(unsealed_private_key, unsealed_private_key + unsealed_key_length);
    printf("owner private key: %s\n", state.owner_private_key.c_str());

    //
    // TODO: BITCOIN
    // set owner_public_key & owner_address
    // 
    const unsigned char *owner_private_key = reinterpret_cast<const unsigned char *>(state.owner_private_key.c_str());
    
    // initialize ECC State for Bitcoin Library
    initializeECCState();

    CKey key;
    key.Set(owner_private_key, owner_private_key + 32, true);
    CPubKey pubkey = key.GetPubKey();

    CBitcoinAddress address;
    address.Set(pubkey.GetID());

    std::string generated_address = address.ToString();
    std::string generated_public_key = HexStr(pubkey);
    printf("owner private key: %s\n", owner_private_key);
    state.owner_public_key = generated_public_key;
    state.owner_address = generated_address;

    state.block_number = 0;

    printf("ecall_load_owner_key.generated_private_key: %s\n", CBitcoinSecret(key).ToString().c_str());
    printf("ecall_load_owner_key.generated_public_key: %s\n", generated_public_key.c_str());
    printf("ecall_load_owner_key.generated_address: %s\n", generated_address.c_str());

    // rouTEE host's public key for verification
    // state.verify_keys["host"] = ;
    // char byteArray[] = {}
    // std::string s(byteArray, sizeof(byteArray));

    // char *_input = "MFYwEAYHKoZIzj0CAQYFK4EEAAoDQgAEpDe2hkjA3LeG8sjcGrBSfAIWxCXlIHQya9Apb7xR8Xjpe0bDWrPkrjZ38Dcqx0T3INM9UB+adVWE3hzduzR9qA==";
    // unsigned char *input = reinterpret_cast<unsigned char *>(_input);
    // unsigned char pubkey[88];
    // size_t pubkeylen;

    // ret = mbedtls_base64_decode( pubkey, 88, &pubkeylen, input, strlen(_input) );


    return NO_ERROR;
}

int ecall_seal_state(char* sealed_state, int* sealed_state_len) {

    // make state as a string
    string state_str = state.to_string();

    // seal the state
    uint32_t sealed_data_size = sgx_calc_sealed_data_size(0, (uint32_t)state_str.length());
    *sealed_state_len = sealed_data_size;
    if (sealed_data_size == UINT32_MAX) {
        return ERR_SGX_ERROR_UNEXPECTED;
    }
    sgx_sealed_data_t *sealed_state_buffer = (sgx_sealed_data_t *) malloc(sealed_data_size);
    sgx_status_t status = sgx_seal_data(0, NULL, (uint32_t)state_str.length(), (uint8_t *) state_str.c_str(), sealed_data_size, sealed_state_buffer);
    if (status != SGX_SUCCESS) {
        return ERR_SEAL_FAILED;
    }

    // copy sealed state to the app buffer
    memcpy(sealed_state, sealed_state_buffer, sealed_data_size);
    free(sealed_state_buffer);
    return NO_ERROR;
}

int ecall_load_state(const char* sealed_state, int sealed_state_len) {
    // for edge8r
    (void) sealed_state_len;

    // unseal the sealed private key
    uint32_t unsealed_state_length = sgx_get_encrypt_txt_len((const sgx_sealed_data_t *) sealed_state);
    uint8_t unsealed_state[unsealed_state_length];
    sgx_status_t status = sgx_unseal_data((const sgx_sealed_data_t *) sealed_state, NULL, 0, unsealed_state, &unsealed_state_length);
    if (status != SGX_SUCCESS) {
        return ERR_UNSEAL_FAILED;
    }

    // load global state
    // state.owner_private_key.assign(unsealed_private_key, unsealed_private_key + unsealed_key_length);
    // // printf("owner private key: %s\n", state.owner_private_key.c_str());
    string state_str;
    state_str.assign(unsealed_state, unsealed_state + unsealed_state_length);
    state.from_string(state_str);

    // printf("success loading state!\n");

    return NO_ERROR;
}

void ecall_seal_channels() {
    // https://github.com/intel/linux-sgx/blob/master/SampleCode/SealUnseal/Enclave_Seal/Enclave_Seal.cpp
    // https://github.com/intel/linux-sgx/blob/master/SampleCode/SealUnseal/App/App.cpp
}

void ecall_unseal_channels() {
    
}
