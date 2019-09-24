#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <electionguard/keyceremony/trustee.h>
#include <electionguard/max_values.h>

#include "keyceremony/message_reps.h"
#include "serialize/keyceremony.h"
#include "serialize/trustee_state.h"
#include "trustee_state_rep.h"

struct KeyCeremony_Trustee_s
{
    uint32_t num_trustees;
    uint32_t threshold;
    uint32_t index;
    //@secret the private key must not be leaked from the system
    struct private_key private_key;
    struct public_key public_keys[MAX_TRUSTEES];
};

struct KeyCeremony_Trustee_new_r KeyCeremony_Trustee_new(uint32_t num_trustees,
                                                         uint32_t threshold,
                                                         uint32_t index)
{
    struct KeyCeremony_Trustee_new_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    if (!(1 <= threshold && threshold <= num_trustees &&
          num_trustees <= MAX_TRUSTEES))
        result.status = KEYCEREMONY_TRUSTEE_INVALID_PARAMS;

    // Allocate the trustee
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        result.trustee = malloc(sizeof(struct KeyCeremony_Trustee_s));
        if (result.trustee == NULL)
            result.status = KEYCEREMONY_TRUSTEE_INSUFFICIENT_MEMORY;
    }

    // Initialize the trustee
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        result.trustee->num_trustees = num_trustees;
        result.trustee->threshold = threshold;
        result.trustee->index = index;

        Crypto_private_key_init(&result.trustee->private_key, threshold);
        for(int i=0; i<threshold; i++){
            Crypto_public_key_new(&result.trustee->public_keys[i], threshold);
        }
    }

    return result;
}

void KeyCeremony_Trustee_free(KeyCeremony_Trustee t) {
    Crypto_private_key_free(&t->private_key, t->threshold);
    for(int i=0; i<t->threshold; i++){
        Crypto_public_key_free(&t->public_keys[i], t->threshold);
    }
    free(t);
}

struct KeyCeremony_Trustee_generate_key_r
KeyCeremony_Trustee_generate_key(KeyCeremony_Trustee t, raw_hash base_hash_code)
{
    struct KeyCeremony_Trustee_generate_key_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    // Generate the keypair
    struct Crypto_gen_keypair_r crypto_result =
        Crypto_gen_keypair(t->threshold, base_hash_code);
    // check that we generated good proofs (right now this call crashes if the proofs fail)
    Crypto_check_keypair_proof(crypto_result.public_key, base_hash_code);
    switch (crypto_result.status)
    {
    case CRYPTO_INSUFFICIENT_MEMORY:
        result.status = KEYCEREMONY_TRUSTEE_INSUFFICIENT_MEMORY;
        break;
    case CRYPTO_SUCCESS:
        break;
    default:
        //@ assert false;
        assert(false && "unreachable");
    };

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        Crypto_private_key_copy(&t->private_key, &crypto_result.private_key);
        Crypto_public_key_copy(&t->public_keys[t->index],
                               &crypto_result.public_key);

        printf("Trustee %d generated public key:\n", t->index);
        print_base16(t->public_keys[t->index].coef_commitments[0]);
        // printf("Trustee %d generated private key:\n", t->index);
        // print_base16(t->private_key.coefficients[0]);
    }

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        // Build the message
        struct key_generated_rep message_rep;

        message_rep.trustee_index = t->index;
        message_rep.public_key.threshold = t->threshold;
        Crypto_public_key_new(&message_rep.public_key,t->threshold);
        Crypto_public_key_copy(&message_rep.public_key,
                               &t->public_keys[t->index]);

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_key_generated(&state, &message_rep);
        Serialize_allocate(&state);
        Serialize_write_key_generated(&state, &message_rep);

        Crypto_public_key_free(&message_rep.public_key, t->threshold);
        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.message = (struct key_generated_message){
                .len = state.len,
                .bytes = state.buf,
            };
        }
    }

    return result;
}

struct KeyCeremony_Trustee_generate_shares_r
KeyCeremony_Trustee_generate_shares(KeyCeremony_Trustee t,
                                    struct all_keys_received_message in_message)
{
    struct KeyCeremony_Trustee_generate_shares_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    struct all_keys_received_rep in_message_rep;

    // Deserialize the input
    {
        struct serialize_state state = {
            .status = SERIALIZE_STATE_READING,
            .len = in_message.len,
            .offset = 0,
            .buf = (uint8_t *)in_message.bytes,
        };

        for(int i=0; i<t->num_trustees; i++){
            Crypto_public_key_new(&in_message_rep.public_keys[i],t->threshold);
        }
        Serialize_read_all_keys_received(&state, &in_message_rep);

        if (state.status != SERIALIZE_STATE_READING)
            result.status = KEYCEREMONY_TRUSTEE_DESERIALIZE_ERROR;
    }

    // Check that my public key is present at t->index
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
        if (!Crypto_public_key_equal(&in_message_rep.public_keys[t->index],
                                     &t->public_keys[t->index]))
            result.status = KEYCEREMONY_TRUSTEE_MISSING_PUBLIC_KEY;

    // Copy other public keys into my state
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
        for (uint32_t i = 0; i < t->num_trustees; i++){
            Crypto_public_key_copy(&t->public_keys[i],
                                   &in_message_rep.public_keys[i]);
            Crypto_public_key_free(&in_message_rep.public_keys[i], t->threshold);
        }

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        // Build the message
        struct shares_generated_rep out_message_rep;

        out_message_rep.trustee_index = t->index;
        out_message_rep.num_trustees = t->num_trustees;
        //re-add for thresholding
        //for (uint32_t i = 0; i < t->num_trustees; i++)
        // {
        //     Crypto_private_key_init(&out_message_rep.shares[i].private_key, t->threshold);
        //     Crypto_private_key_copy(&out_message_rep.shares[i].private_key,
        //                             &t->private_key);
        //     Crypto_public_key_init(&out_message_rep.shares[i].recipient_public_key, t->threshold);
        //     Crypto_public_key_copy(
        //         &out_message_rep.shares[i].recipient_public_key,
        //         &t->public_keys[i]);
        // }

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_shares_generated(&state, &out_message_rep);
        Serialize_allocate(&state);
        Serialize_write_shares_generated(&state, &out_message_rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.message = (struct shares_generated_message){
                .len = state.len,
                .bytes = state.buf,
            };
        }
    }
    return result;
}

struct KeyCeremony_Trustee_verify_shares_r
KeyCeremony_Trustee_verify_shares(KeyCeremony_Trustee t,
                                  struct all_shares_received_message in_message)
{
    struct KeyCeremony_Trustee_verify_shares_r result = {
        .status = KEYCEREMONY_TRUSTEE_SUCCESS,
    };

    struct all_shares_received_rep in_message_rep;

    // Deserialize the input
    {
        struct serialize_state state = {
            .status = SERIALIZE_STATE_READING,
            .len = in_message.len,
            .offset = 0,
            .buf = (uint8_t *)in_message.bytes,
        };

        // for(int i=0; i<t->num_trustees; i++){
        //     for(int j=0; j<t->num_trustees; j++){
        //         Crypto_encrypted_key_share_init(&in_message_rep.shares[i][j], t->threshold);
        //     }
        // }
        Serialize_read_all_shares_received(&state, &in_message_rep);

        if (state.status != SERIALIZE_STATE_READING)
            result.status = KEYCEREMONY_TRUSTEE_DESERIALIZE_ERROR;
    }

    // Check that all the shares meant for me match the public keys
    // previously received
    for (uint32_t i = 0; i < t->threshold; i++)
    {
        /* Disabled since we aren't using shares for now
        struct encrypted_key_share share = in_message_rep.shares[t->index][i];
        if (!Crypto_public_key_equal(&share.recipient_public_key,
                                     &t->public_keys[i]))
            result.status = KEYCEREMONY_TRUSTEE_INVALID_KEY_SHARE;
        */
    }

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        // Build the message
        struct shares_verified_rep out_message_rep = {
            .trustee_index = t->index,
            .verified = true,
        };

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_shares_verified(&state, &out_message_rep);
        Serialize_allocate(&state);
        Serialize_write_shares_verified(&state, &out_message_rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.message = (struct shares_verified_message){
                .len = state.len,
                .bytes = state.buf,
            };
        }
        // for(int i=0; i<t->num_trustees; i++){
        //     for(int j=0; j<t->num_trustees; j++){
        //         Crypto_encrypted_key_share_free(&in_message_rep.shares[i][j], t->threshold);
        //     }
        // }
    }

    return result;
}

struct KeyCeremony_Trustee_export_state_r
KeyCeremony_Trustee_export_state(KeyCeremony_Trustee t)
{
    struct KeyCeremony_Trustee_export_state_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    {
        struct trustee_state_rep rep;
        rep.index = t->index;
        Crypto_private_key_init(&rep.private_key,t->threshold);
        Crypto_private_key_copy(&rep.private_key, &t->private_key);

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_trustee_state(&state, &rep);
        Serialize_allocate(&state);
        Serialize_write_trustee_state(&state, &rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.state = (struct trustee_state){
                .len = state.len,
                .bytes = state.buf,
            };
        }

        Crypto_private_key_free(&rep.private_key, t->threshold);
    }

    return result;
}
