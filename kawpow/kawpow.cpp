#include "stratum.h"

#include <openssl/sha.h>

#include <kawpow/hash.h>
#include <kawpow/helpers.hpp>
#include <kawpow/include/ethash/progpow.hpp>

static int g_extraonce1_counter = 0;

void get_nonce_prefix(char* extraonce1)
{
    CommonLock(&g_nonce1_mutex);

    g_extraonce1_counter++;
    sprintf(extraonce1, "%06x", g_extraonce1_counter | 0x800000);

    CommonUnlock(&g_nonce1_mutex);
}

bool kawpow_send_nonceprefix(YAAMP_CLIENT* client)
{
    char buffer[256];
    memset(buffer, 0, sizeof(buffer));

    sprintf(buffer, "{\"id\":1,\"error\":null,\"result\":[null,\"%s\"]}\n", client->extranonce1);
    return socket_send(client->sock, buffer);
}

bool kawpow_check_nonceprefix(YAAMP_CLIENT* client, std::string nonce)
{
    char submitted_nonce[20];
    sprintf(submitted_nonce, "%s", nonce.c_str());
    submitted_nonce[6] = '\0';

    bool match = memcmp(client->extranonce1, submitted_nonce, 6);
    return !match;
}

uint256 get_kawpow_seed(int height)
{
    std::string seed_string = to_hex(ethash::calculate_epoch_seed(ethash::get_epoch_number(height)));
    return uint256S(seed_string);
}

void kawpow_job_mining_notify_buffer(YAAMP_JOB* job, YAAMP_CLIENT* client, char* buffer)
{
    YAAMP_JOB_TEMPLATE* templ = job->templ;

    sprintf(buffer, "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"%x\",\"%s\",\"%s\",\"%s\",true,%d,\"%s\"]}\n",
        job->id,
        templ->header_hash.ToString().c_str(),
        templ->header_seed.ToString().c_str(),
        client->share_target.ToString().c_str(),
        templ->height,
        templ->nbits);
}

void build_kawpow_header(YAAMP_JOB_TEMPLATE* templ, const char* nonce64, const char* mixhash, char* blockheader_be, char* coinbase)
{
    YAAMP_JOB_VALUES submitvalues;
    memset(&submitvalues, 0, sizeof(submitvalues));

    sprintf(submitvalues.coinbase, "%s%s", templ->coinb1, templ->coinb2);
    int coinbase_len = strlen(submitvalues.coinbase);
    memcpy(coinbase, submitvalues.coinbase, coinbase_len);

    unsigned char coinbase_bin[1024];
    memset(coinbase_bin, 0, 1024);
    binlify(coinbase_bin, submitvalues.coinbase);

    char doublehash[128];
    memset(doublehash, 0, 128);

    YAAMP_HASH_FUNCTION merkle_hash = sha256_double_hash_hex;
    if (g_current_algo->merkle_func)
        merkle_hash = g_current_algo->merkle_func;
    merkle_hash((char*)coinbase_bin, doublehash, coinbase_len / 2);

    string merkleroot = merkle_with_first(templ->txsteps, doublehash);
    ser_string_be(merkleroot.c_str(), submitvalues.merkleroot_be, 8);

    ///////////////////////////////////////////// STFIX
    char ntime[10];
    memset(ntime, 0, 10);
    sprintf(ntime, "%08x", templ->created);

    char n1[10], n2[10];
    memset(n1, 0, 10);
    memset(n2, 0, 10);
    memcpy(n2, nonce64, 8);
    memcpy(n1, nonce64 + 8, 8);

    char mixhash_be[128];
    memset(mixhash_be, 0, 128);
    ser_string_be2(mixhash, mixhash_be, 8);
    ///////////////////////////////////////////// ENDFIX

    char blockheader[512];
    memset(blockheader, 0, 512);
    sprintf(blockheader, "%s%s%s%s%s%08x%s%s%s", templ->version, templ->prevhash_be, submitvalues.merkleroot_be,
        ntime, templ->nbits, templ->height, n1, n2, mixhash_be);

    ser_string_be(blockheader, blockheader_be, 30);
}

uint256 build_header_hash(YAAMP_JOB_TEMPLATE* templ)
{
    YAAMP_JOB_VALUES submitvalues;
    memset(&submitvalues, 0, sizeof(submitvalues));
    sprintf(submitvalues.coinbase, "%s%s", templ->coinb1, templ->coinb2);
    int coinbase_len = strlen(submitvalues.coinbase);

    unsigned char coinbase_bin[1024];
    memset(coinbase_bin, 0, 1024);
    binlify(coinbase_bin, submitvalues.coinbase);

    char doublehash[128];
    memset(doublehash, 0, 128);

    YAAMP_HASH_FUNCTION merkle_hash = sha256_double_hash_hex;
    if (g_current_algo->merkle_func)
        merkle_hash = g_current_algo->merkle_func;
    merkle_hash((char*)coinbase_bin, doublehash, coinbase_len / 2);

    string merkleroot = merkle_with_first(templ->txsteps, doublehash);
    ser_string_be(merkleroot.c_str(), submitvalues.merkleroot_be, 8);

    ///////////////////////////////////////////// STFIX
    char ntime[10];
    memset(ntime, 0, 10);
    sprintf(ntime, "%08x", templ->created);
    ///////////////////////////////////////////// ENDFIX

    sprintf(submitvalues.header, "%s%s%s%s%s%08x", templ->version, templ->prevhash_be, submitvalues.merkleroot_be,
        ntime, templ->nbits, templ->height);

    ser_string_be(submitvalues.header, submitvalues.header_be, 20);
    binlify(submitvalues.header_bin, submitvalues.header_be);

    uint256 header_hash;
    SHA256(submitvalues.header_bin, 80, (unsigned char*)&header_hash);
    SHA256((const unsigned char*)&header_hash, 32, (unsigned char*)&header_hash);

    return header_hash;
}

static void kawpow_block(YAAMP_CLIENT* client, YAAMP_JOB* job, const char* nonce64, const char* mixhash)
{
    YAAMP_COIND* coind = job->coind;
    YAAMP_JOB_TEMPLATE* templ = job->templ;

    if (job->block_found)
        return;
    if (job->deleted)
        return;

    int block_size = YAAMP_SMALLBUFSIZE;
    vector<string>::const_iterator i;

    for (i = templ->txdata.begin(); i != templ->txdata.end(); ++i)
        block_size += strlen((*i).c_str());

    char* block_hex = (char*)malloc(block_size);
    if (!block_hex)
        return;

    char block_header[256];
    memset(block_header, 0, sizeof(block_header));

    char coin_base[256];
    memset(coin_base, 0, sizeof(coin_base));

    build_kawpow_header(templ, nonce64, mixhash, block_header, coin_base);

    {
        char count_hex[8] = { 0 };
        if (templ->txcount <= 252)
            sprintf(count_hex, "%02x", templ->txcount & 0xFF);
        else
            sprintf(count_hex, "fd%02x%02x", templ->txcount & 0xFF, templ->txcount >> 8);

        memset(block_hex, 0, block_size);
        sprintf(block_hex, "%s%s%s", block_header, count_hex, coin_base);

        vector<string>::const_iterator i;
        for (i = templ->txdata.begin(); i != templ->txdata.end(); ++i)
            sprintf(block_hex + strlen(block_hex), "%s", (*i).c_str());

        bool b = coind_submit(coind, block_hex);
        if (b) {
            debuglog("*** ACCEPTED %s %d by %s (id: %d)\n", coind->name, templ->height,
                client->sock->ip, client->userid);
            job->block_found = true;
            job_signal();
        } else {
            debuglog("*** REJECTED :( %s block %d %d txs\n", coind->name, templ->height, templ->txcount);
            rejectlog("REJECTED %s block %d\n", coind->symbol, templ->height);
        }
    }

    free(block_hex);
}

bool kawpow_submit(YAAMP_CLIENT* client, json_value* json_params)
{
    // submit(worker_name, jobid, nonce, header, mixhash):
    if (json_params->u.array.length < 5 || !valid_string_params(json_params)) {
        debuglog("%s - %s bad message\n", client->username, client->sock->ip);
        client->submit_bad++;
        return false;
    }

    char nonce[128], header[128], mixhash[128];
    memset(nonce, 0, sizeof(nonce));
    memset(header, 0, sizeof(header));
    memset(mixhash, 0, sizeof(mixhash));

    if (strlen(json_params->u.array.values[1]->u.string.ptr) > 32) {
        clientlog(client, "bad json, wrong jobid len");
        client->submit_bad++;
        return false;
    }

    int jobid = htoi(json_params->u.array.values[1]->u.string.ptr);

    strncpy(nonce, json_params->u.array.values[2]->u.string.ptr + 2, 16);
    strncpy(header, json_params->u.array.values[3]->u.string.ptr + 2, 64);
    strncpy(mixhash, json_params->u.array.values[4]->u.string.ptr + 2, 64);

    string_lower(nonce);
    string_lower(header);
    string_lower(mixhash);

    YAAMP_JOB* job = (YAAMP_JOB*)object_find(&g_list_job, jobid, true);
    if (!job) {
        client_submit_error(client, NULL, 21, "Invalid job id", header, mixhash, nonce);
        return true;
    } else if (job->deleted) {
        client_send_result(client, "true");
        object_unlock(job);
        return true;
    }

    //! sanity check nonce
    if (strlen(nonce) != YAAMP_NONCE_SIZE * 2 || !ishexa(nonce, YAAMP_NONCE_SIZE * 2)) {
        client_submit_error(client, job, 20, "Invalid nonce size", header, mixhash, nonce);
        return true;
    }

    //! make sure nonce matches prefix
    if (!kawpow_check_nonceprefix(client, nonce)) {
        client_submit_error(client, job, 20, "Invalid nonce prefix", header, mixhash, nonce);
        return true;
    }

    //! sanity check header/mixhash
    if ((strlen(header) != 64 || !ishexa(header, 64)) || (strlen(mixhash) != 64 || !ishexa(mixhash, 64))) {
        client_submit_error(client, job, 20, "Invalid header/mixhash", header, mixhash, nonce);
        return true;
    }

    YAAMP_SHARE* share = share_find(job->id, header, mixhash, nonce, client->username);
    if (share) {
        client_submit_error(client, job, 22, "Duplicate share", header, mixhash, nonce);
        return true;
    }

    std::string header_str = string(header);
    std::string mixhash_str = string(mixhash);
    std::string nonce_str = string(nonce);

    std::string mixhash_calc;
    uint256 hash = kawpow_hash(header_str, nonce_str, mixhash_calc);

    uint256 target = client->share_target;
    uint64_t hash_int = get_hash_difficulty((uint8_t*)&hash);
    double share_diff = diff_to_target(hash_int);

    // stratumlog("client %d on job %d sent powhash %s\n", client->id, job->id, hash.ToString().c_str());

    if (hash > target) {
        client_submit_error(client, job, 26, "Low difficulty share", header, mixhash, nonce);
        return true;
    }

    YAAMP_JOB_TEMPLATE* templ = job->templ;
    uint32_t nbits = strtoul(templ->nbits, NULL, 16);

    uint256 coin_target;
    decode_nbits(coin_target, nbits);

    if (hash < coin_target) {
        kawpow_block(client, job, nonce, mixhash);
    }

    client_send_result(client, "true");
    client_record_difficulty(client);
    client->submit_bad = 0;
    client->shares++;

    share_add(client, job, true, header, mixhash, nonce, share_diff, 0);
    object_unlock(job);

    return true;
}
